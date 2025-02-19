// Copyright (c) 2015-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <httprpc.h>

#include <common/args.h>
#include <crypto/hmac_sha256.h>
#include <httpserver.h>
#include <logging.h>
#include <netaddress.h>
#include <rpc/protocol.h>
#include <rpc/server.h>
#include <util/fs.h>
#include <util/fs_helpers.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <walletinitinterface.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

/** WWW-Authenticate to present with 401 Unauthorized response */
static const char* WWW_AUTH_HEADER_DATA = "Basic realm=\"jsonrpc\"";

/** Simple one-shot callback timer to be used by the RPC mechanism to e.g.
 * re-lock the wallet.
 */
class HTTPRPCTimer : public RPCTimerBase
{
public:
    HTTPRPCTimer(struct event_base* eventBase, std::function<void()>& func, int64_t millis) :
        ev(eventBase, false, func)
    {
        struct timeval tv;
        tv.tv_sec = millis/1000;
        tv.tv_usec = (millis%1000)*1000;
        ev.trigger(&tv);
    }
private:
    HTTPEvent ev;
};

class HTTPRPCTimerInterface : public RPCTimerInterface
{
public:
    explicit HTTPRPCTimerInterface(struct event_base* _base) : base(_base)
    {
    }
    const char* Name() override
    {
        return "HTTP";
    }
    RPCTimerBase* NewTimer(std::function<void()>& func, int64_t millis) override
    {
        return new HTTPRPCTimer(base, func, millis);
    }
private:
    struct event_base* base;
};


/* Pre-base64-encoded authentication token */
static std::string strRPCUserColonPass;
/* Stored RPC timer interface (for unregistration) */
static std::unique_ptr<HTTPRPCTimerInterface> httpRPCTimerInterface;
/* List of -rpcauth values */
static std::vector<std::vector<std::string>> g_rpcauth;
/* RPC Auth Whitelist */
static std::map<std::string, std::set<std::string>> g_rpc_whitelist;
static bool g_rpc_whitelist_default = false;

static void JSONErrorReply(HTTPRequest* req, const UniValue& objError, const UniValue& id)
{
    // Send error reply from json-rpc error object
    int nStatus = HTTP_INTERNAL_SERVER_ERROR;
    int code = objError.find_value("code").getInt<int>();

    if (code == RPC_INVALID_REQUEST)
        nStatus = HTTP_BAD_REQUEST;
    else if (code == RPC_METHOD_NOT_FOUND)
        nStatus = HTTP_NOT_FOUND;

    std::string strReply = JSONRPCReply(NullUniValue, objError, id);

    req->WriteHeader("Content-Type", "application/json");
    req->WriteReply(nStatus, strReply);
}

//This function checks username and password against -rpcauth
//entries from config file.
static bool multiUserAuthorized(std::string strUserPass, std::string& out_wallet_restriction)
{
    if (strUserPass.find(':') == std::string::npos) {
        return false;
    }
    std::string strUser = strUserPass.substr(0, strUserPass.find(':'));
    std::string strPass = strUserPass.substr(strUserPass.find(':') + 1);

    for (const auto& vFields : g_rpcauth) {
        std::string strName = vFields[0];
        if (!TimingResistantEqual(strName, strUser)) {
            continue;
        }

        std::string strSalt = vFields[1];
        std::string strHash = vFields[2];

        static const unsigned int KEY_SIZE = 32;
        unsigned char out[KEY_SIZE];

        CHMAC_SHA256(reinterpret_cast<const unsigned char*>(strSalt.data()), strSalt.size()).Write(reinterpret_cast<const unsigned char*>(strPass.data()), strPass.size()).Finalize(out);
        std::vector<unsigned char> hexvec(out, out+KEY_SIZE);
        std::string strHashFromPass = HexStr(hexvec);

        if (TimingResistantEqual(strHashFromPass, strHash)) {
            out_wallet_restriction = (vFields.size() > 3) ? vFields[3] : "";
            return true;
        }
    }
    return false;
}

static bool RPCAuthorized(const std::string& strAuth, std::string& strAuthUsernameOut, std::string& out_wallet_restriction)
{
    if (strRPCUserColonPass.empty()) // Belt-and-suspenders measure if InitRPCAuthentication was not called
        return false;
    if (strAuth.substr(0, 6) != "Basic ")
        return false;
    std::string_view strUserPass64 = TrimStringView(std::string_view{strAuth}.substr(6));
    auto userpass_data = DecodeBase64(strUserPass64);
    std::string strUserPass;
    if (!userpass_data) return false;
    strUserPass.assign(userpass_data->begin(), userpass_data->end());

    if (strUserPass.find(':') != std::string::npos)
        strAuthUsernameOut = strUserPass.substr(0, strUserPass.find(':'));

    //Check if authorized under single-user field
    if (TimingResistantEqual(strUserPass, strRPCUserColonPass)) {
        out_wallet_restriction = "";
        return true;
    }
    return multiUserAuthorized(strUserPass, out_wallet_restriction);
}

static bool HTTPReq_JSONRPC(const std::any& context, HTTPRequest* req)
{
    // JSONRPC handles only POST
    if (req->GetRequestMethod() != HTTPRequest::POST) {
        req->WriteReply(HTTP_BAD_METHOD, "JSONRPC server handles only POST requests");
        return false;
    }
    // Check authorization
    std::pair<bool, std::string> authHeader = req->GetHeader("authorization");
    if (!authHeader.first) {
        req->WriteHeader("WWW-Authenticate", WWW_AUTH_HEADER_DATA);
        req->WriteReply(HTTP_UNAUTHORIZED);
        return false;
    }

    JSONRPCRequest jreq;
    jreq.context = context;
    jreq.peerAddr = req->GetPeer().ToStringAddrPort();
    if (!RPCAuthorized(authHeader.second, jreq.authUser, jreq.m_wallet_restriction)) {
        LogPrintf("ThreadRPCServer incorrect password attempt from %s\n", jreq.peerAddr);

        /* Deter brute-forcing
           If this results in a DoS the user really
           shouldn't have their RPC port exposed. */
        UninterruptibleSleep(std::chrono::milliseconds{250});

        req->WriteHeader("WWW-Authenticate", WWW_AUTH_HEADER_DATA);
        req->WriteReply(HTTP_UNAUTHORIZED);
        return false;
    }

    try {
        // Parse request
        UniValue valRequest;
        if (!valRequest.read(req->ReadBody()))
            throw JSONRPCError(RPC_PARSE_ERROR, "Parse error");

        // Set the URI
        jreq.URI = req->GetURI();

        std::string strReply;
        bool user_has_whitelist = g_rpc_whitelist.count(jreq.authUser);
        if (!user_has_whitelist && g_rpc_whitelist_default) {
            LogPrintf("RPC User %s not allowed to call any methods\n", jreq.authUser);
            req->WriteReply(HTTP_FORBIDDEN);
            return false;

        // singleton request
        } else if (valRequest.isObject()) {
            jreq.parse(valRequest);
            if (user_has_whitelist && !g_rpc_whitelist[jreq.authUser].count(jreq.strMethod)) {
                LogPrintf("RPC User %s not allowed to call method %s\n", jreq.authUser, jreq.strMethod);
                req->WriteReply(HTTP_FORBIDDEN);
                return false;
            }
            UniValue result = tableRPC.execute(jreq);

            // Send reply
            strReply = JSONRPCReply(result, NullUniValue, jreq.id);

        // array of requests
        } else if (valRequest.isArray()) {
            if (user_has_whitelist) {
                for (unsigned int reqIdx = 0; reqIdx < valRequest.size(); reqIdx++) {
                    if (!valRequest[reqIdx].isObject()) {
                        throw JSONRPCError(RPC_INVALID_REQUEST, "Invalid Request object");
                    } else {
                        const UniValue& request = valRequest[reqIdx].get_obj();
                        // Parse method
                        std::string strMethod = request.find_value("method").get_str();
                        if (!g_rpc_whitelist[jreq.authUser].count(strMethod)) {
                            LogPrintf("RPC User %s not allowed to call method %s\n", jreq.authUser, strMethod);
                            req->WriteReply(HTTP_FORBIDDEN);
                            return false;
                        }
                    }
                }
            }
            strReply = JSONRPCExecBatch(jreq, valRequest.get_array());
        }
        else
            throw JSONRPCError(RPC_PARSE_ERROR, "Top-level object parse error");

        req->WriteHeader("Content-Type", "application/json");
        req->WriteReply(HTTP_OK, strReply);
    } catch (const UniValue& objError) {
        JSONErrorReply(req, objError, jreq.id);
        return false;
    } catch (const std::exception& e) {
        JSONErrorReply(req, JSONRPCError(RPC_PARSE_ERROR, e.what()), jreq.id);
        return false;
    }
    return true;
}

static bool InitRPCAuthentication()
{
    if (gArgs.GetArg("-rpcpassword", "") == "")
    {
        LogPrintf("Using random cookie authentication.\n");

        std::optional<fs::perms> cookie_perms{DEFAULT_COOKIE_PERMS};
        auto cookie_perms_arg{gArgs.GetArg("-rpccookieperms")};
        if (cookie_perms_arg) {
            if (*cookie_perms_arg == "0") {
                cookie_perms = std::nullopt;
            } else if (cookie_perms_arg->empty() || *cookie_perms_arg == "1") {
                // leave at default
            } else {
                auto perm_opt = StringToPerms(*cookie_perms_arg);
                if (!perm_opt) {
                    LogInfo("Invalid -rpccookieperms=%s; must be one of 'owner', 'group', or 'all'.\n", *cookie_perms_arg);
                    return false;
                }
                cookie_perms = *perm_opt;
            }
        }

        if (!GenerateAuthCookie(&strRPCUserColonPass, std::make_pair(cookie_perms, bool(cookie_perms_arg)))) {
            return false;
        }
    } else {
        LogPrintf("Config options rpcuser and rpcpassword will soon be deprecated. Locally-run instances may remove rpcuser to use cookie-based auth, or may be replaced with rpcauth. Please see share/rpcauth for rpcauth auth generation.\n");
        strRPCUserColonPass = gArgs.GetArg("-rpcuser", "") + ":" + gArgs.GetArg("-rpcpassword", "");
    }
    constexpr auto AddRPCAuth = [](const std::string& rpcauth) {
        std::vector<std::string> fields{SplitString(rpcauth, ':')};
        if (fields.size() < 2 || fields.size() > 3) {
            return false;
        }
        const std::vector<std::string> salt_hmac{SplitString(fields[1], '$')};
        if (salt_hmac.size() == 2) {
            fields.erase(fields.begin() + 1);
            fields.insert(fields.begin() + 1, salt_hmac.begin(), salt_hmac.end());
            g_rpcauth.push_back(fields);
        } else {
            return false;
        }
        return true;
    };
    if (!(gArgs.IsArgNegated("-rpcauth") || (gArgs.GetArgs("-rpcauth").empty() && gArgs.GetArgs("-rpcauthfile").empty()))) {
        LogPrintf("Using rpcauth authentication.\n");
        for (const std::string& rpcauth : gArgs.GetArgs("-rpcauth")) {
            if (rpcauth.empty()) continue;
            if (!AddRPCAuth(rpcauth)) {
                LogPrintf("Invalid -rpcauth argument.\n");
                return false;
            }
        }
        for (const std::string& path : gArgs.GetArgs("-rpcauthfile")) {
            std::ifstream file;
            file.open(path);
            if (!file.is_open()) continue;
            std::string rpcauth;
            size_t lineno = 0;
            while (std::getline(file, rpcauth)) {
                ++lineno;
                if (!AddRPCAuth(rpcauth)) {
                    LogPrintf("WARNING: Invalid line %s in -rpcauthfile=%s; ignoring\n", lineno, path);
                }
            }
        }
    }

    g_rpc_whitelist_default = gArgs.GetBoolArg("-rpcwhitelistdefault", gArgs.IsArgSet("-rpcwhitelist"));
    for (const std::string& strRPCWhitelist : gArgs.GetArgs("-rpcwhitelist")) {
        auto pos = strRPCWhitelist.find(':');
        std::string strUser = strRPCWhitelist.substr(0, pos);
        bool intersect = g_rpc_whitelist.count(strUser);
        std::set<std::string>& whitelist = g_rpc_whitelist[strUser];
        if (pos != std::string::npos) {
            std::string strWhitelist = strRPCWhitelist.substr(pos + 1);
            std::vector<std::string> whitelist_split = SplitString(strWhitelist, ", ");
            std::set<std::string> new_whitelist{
                std::make_move_iterator(whitelist_split.begin()),
                std::make_move_iterator(whitelist_split.end())};
            if (intersect) {
                std::set<std::string> tmp_whitelist;
                std::set_intersection(new_whitelist.begin(), new_whitelist.end(),
                       whitelist.begin(), whitelist.end(), std::inserter(tmp_whitelist, tmp_whitelist.end()));
                new_whitelist = std::move(tmp_whitelist);
            }
            whitelist = std::move(new_whitelist);
        }
    }

    return true;
}

bool StartHTTPRPC(const std::any& context)
{
    LogPrint(BCLog::RPC, "Starting HTTP RPC server\n");
    if (!InitRPCAuthentication())
        return false;

    auto handle_rpc = [context](HTTPRequest* req, const std::string&) { return HTTPReq_JSONRPC(context, req); };
    RegisterHTTPHandler("/", true, handle_rpc);
    if (g_wallet_init_interface.HasWalletSupport()) {
        RegisterHTTPHandler("/wallet/", false, handle_rpc);
    }
    struct event_base* eventBase = EventBase();
    assert(eventBase);
    httpRPCTimerInterface = std::make_unique<HTTPRPCTimerInterface>(eventBase);
    RPCSetTimerInterface(httpRPCTimerInterface.get());
    return true;
}

void InterruptHTTPRPC()
{
    LogPrint(BCLog::RPC, "Interrupting HTTP RPC server\n");
}

void StopHTTPRPC()
{
    LogPrint(BCLog::RPC, "Stopping HTTP RPC server\n");
    UnregisterHTTPHandler("/", true);
    if (g_wallet_init_interface.HasWalletSupport()) {
        UnregisterHTTPHandler("/wallet/", false);
    }
    if (httpRPCTimerInterface) {
        RPCUnsetTimerInterface(httpRPCTimerInterface.get());
        httpRPCTimerInterface.reset();
    }
}

std::set<std::string> GetWhitelistedRpcs(const std::string& user_name)
{
    if (auto it = g_rpc_whitelist.find(user_name); it != g_rpc_whitelist.end()) {
        return it->second;
    }
    if (g_rpc_whitelist_default) {
        return std::set<std::string>();
    }

    // Build a list of every method
    std::set<std::string> allowed_methods;
    for (const auto& method_name : tableRPC.listCommands()) {
        allowed_methods.insert(method_name);
    }
    return allowed_methods;
}

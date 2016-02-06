#!/bin/sh
# Copyright (c) 2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#
# A helper script to generate source release tarball

export LC_ALL=C
set -ex

[ "$#" -ge 2 ]

GIT_ARCHIVE="$1"
DISTNAME="$2"

git archive --prefix="${DISTNAME}/" HEAD | tar -xp
cd "${DISTNAME}"

./autogen.sh
./configure --prefix=/ --disable-ccache --disable-maintainer-mode --disable-dependency-tracking
make src_files
make distclean

cd ..
tar \
  --format=ustar \
  --exclude autom4te.cache \
  --exclude .deps \
  --exclude .git \
  --sort=name \
  --mode='u+rw,go+r-w,a+X' --owner=0 --group=0 \
  --mtime="${REFERENCE_DATETIME}" \
  -c "${DISTNAME}" | \
  gzip -9n \
  >"${GIT_ARCHIVE}"

rm -rf "${DISTNAME}"

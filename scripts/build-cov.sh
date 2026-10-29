#!/bin/sh
# build-cov.sh [TREE [DEST]]: builds the blktap tree TREE (default $COV_TREE, else /root/cov/bt-cov)
# with gcov instrumentation, and installs it into DEST (default /opt/blktap-cov), with prefix /opt/blktap.
set -ex
TREE=$(realpath ${1:-${COV_TREE:-/root/cov/bt-cov}}); DEST=${2:-/opt/blktap-cov}
STAGE=$(mktemp -d)
. /opt/rh/devtoolset-11/enable
cd $TREE
[ -x configure ] || ./autogen.sh
CFLAGS="-O0 -g --coverage -fprofile-update=atomic -Wno-stringop-truncation" LDFLAGS="--coverage" ./configure --prefix=/opt/blktap
make clean
make -j$(nproc)
make install DESTDIR=$STAGE
rm -rf $DEST
mv $STAGE/opt/blktap $DEST
rm -rf $STAGE

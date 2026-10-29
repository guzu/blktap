#!/bin/sh
# lcov-report.sh HOST OUTDIR: fetches the instrumented build tree of HOST ($COV_TREE, default
# /root/cov/bt-cov: sources, .gcno, .gcda) and captures OUTDIR/cov.info, without any filter
# (see lcov-filter.sh, or lcov-filtered-report.sh which does everything).
# Needs a local gcov-11 (same .gcda format as the gcc 11 of devtoolset-11).
set -e
HOST=$1; OUT=$(realpath -m $2)
TREE=${COV_TREE:-/root/cov/bt-cov}; PARENT=$(dirname $TREE); NAME=$(basename $TREE)
mkdir -p $OUT && cd $OUT && rm -rf $NAME
ssh root@$HOST "cd $PARENT && tar cf - --exclude='*.o' --exclude='*.lo' --exclude='*.a' --exclude='*.so*' $NAME" < /dev/null | tar xf -
IGN=inconsistent,mismatch,unused,source,empty,negative,range
# The system headers of dom0 differ from the local ones: leave /usr out. The .gcno files keep the name of
# the tree at build time, which may have been renamed since: map any tree of $PARENT to the local copy.
lcov -q --capture --directory $NAME --gcov-tool gcov-11 --rc branch_coverage=1 \
    --substitute "s#^$PARENT/[^/]+/#$OUT/$NAME/#" --exclude '/usr/*' --ignore-errors $IGN -o cov.info

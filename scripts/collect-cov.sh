#!/bin/sh
# collect-cov.sh LABEL [TREE]: runs gcov (JSON) on every .gcno of the build tree TREE (default $COV_TREE,
# else /root/cov/bt-cov), from its compile directory, into <parent of TREE>/out-LABEL/N.gcov.json.gz
# (objects without .gcda are reported as not executed)
set -e
GCOV=/opt/rh/devtoolset-11/root/usr/bin/gcov
T=$(realpath ${2:-${COV_TREE:-/root/cov/bt-cov}}); O=$(dirname $T)/out-$1
rm -rf $O; mkdir -p $O; n=0
find $T -name '*.gcno' | sort | while read g; do
    n=$((n+1)); obj=$(dirname $g); src=${obj%/.libs}
    # run from the compile dir (source paths are relative to it); gcov writes the JSON there
    rm -f $src/*.gcov.json.gz
    (cd $src && $GCOV -j -b -o $obj ${g%.gcno}.gcno > /dev/null 2>&1 || true)
    for f in $src/*.gcov.json.gz; do [ -e "$f" ] && mv "$f" $O/$n.gcov.json.gz; done
    echo "$n $src $g" >> $O/index.txt
done
ls $O | wc -l

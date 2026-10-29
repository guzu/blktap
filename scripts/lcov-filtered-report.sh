#!/bin/sh
# lcov-filtered-report.sh HOST OUTDIR [TITLE]: lcov-report.sh, then lcov-filter.sh, then genhtml.
# Writes OUTDIR/cov.info (raw), OUTDIR/cov-filtered.info and OUTDIR/html.
# $FILTERS: rules file (default cov-filters.conf next to this script; FILTERS=/dev/null for none).
# $FILTER_RULES (optional): extra rules separated by ';', e.g. FILTER_RULES='function ^foo$; file drivers/bar.c'.
set -e
D=$(cd "$(dirname "$0")" && pwd); HOST=$1; OUT=$(realpath -m $2); TITLE=${3:-tapdisk coverage}
sh $D/lcov-report.sh $HOST $OUT
cd $OUT; rm -rf html
EXTRA=; IFS=';'; for r in $FILTER_RULES; do EXTRA="$EXTRA -e '$r'"; done; unset IFS
eval sh $D/lcov-filter.sh -f ${FILTERS:-$D/cov-filters.conf} $EXTRA cov.info cov-filtered.info
genhtml -q --rc branch_coverage=1 --ignore-errors inconsistent,source,range --prefix $OUT/$(basename ${COV_TREE:-/root/cov/bt-cov}) \
    --title "$TITLE" --legend --show-details -o html cov-filtered.info
lcov --summary cov-filtered.info --rc branch_coverage=1 2>&1 | grep -E 'lines|functions|branches'

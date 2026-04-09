#!/bin/sh
#
# Build tapdisk-vbd-stress with the default flags and run it under
# valgrind. No special CFLAGS are needed -- the in-tree build is already
# -O0 -g3, which is what valgrind likes.
#
# Any extra arguments are forwarded to tapdisk-vbd-stress, e.g.:
#     ./run-valgrind.sh -n 2000 -i 4
#
# Defaults are tuned to keep the run short (valgrind is ~20x slower than
# native): no injected latency, 2000 requests, 60s wall-clock cap.

set -eu

here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/.." && pwd)

#default_opts="-n 2000 -T 60 -l 0"
default_opts="-T 60"

# Valgrind wants -O0 -g3 (no inlining, full debug). Those are the
# in-tree defaults, so a stock configure + make is enough.
VG_CFLAGS="-O0 -g3"

# If the real ELF (not the libtool wrapper) is missing, do a full
# configure + build from the top of the tree. Otherwise just refresh
# the harness incrementally; if libtapdisk.la is stale, the top-level
# `make` will pull it in via the recursive build.
if [ ! -x "$here/.libs/tapdisk-vbd-stress" ]; then
    echo "run-valgrind.sh: binary missing, doing a full configure + build" >&2
    cd "$top"
    if [ ! -f configure ]; then
        if [ -x ./autogen.sh ]; then
            ./autogen.sh
        else
            autoreconf -fi
        fi
    fi
    ./configure CFLAGS="$VG_CFLAGS"
    make
    cd "$here"
else
    make -C "$here" tapdisk-vbd-stress >/dev/null
fi

# stress/tapdisk-vbd-stress is a libtool wrapper script, not the real
# ELF. Going through `libtool --mode=execute` sets LD_LIBRARY_PATH for
# the in-build-tree .so dependencies and then execs valgrind on the
# real ELF in stress/.libs/.
exec "$top/libtool" --mode=execute \
    valgrind \
        --leak-check=full \
        --show-leak-kinds=definite,indirect \
        --track-origins=yes \
        --error-exitcode=1 \
        --num-callers=30 \
        "$here/tapdisk-vbd-stress" \
        $default_opts \
        "$@"

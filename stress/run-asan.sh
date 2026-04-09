#!/bin/sh
#
# Rebuild the *whole* blktap tree with AddressSanitizer enabled, then
# run tapdisk-vbd-stress under it. ASan requires every translation
# unit linked into the binary to be instrumented, so we cannot just
# rebuild stress/ -- libtapdisk.a, libvhd, libqcow2 etc. all need
# -fsanitize=address.
#
# This script is destructive: it runs `make distclean` and re-runs
# configure with sanitizer flags. The previous build is wiped.
#
# Any extra arguments are forwarded to tapdisk-vbd-stress, e.g.:
#     ./run-asan.sh -n 5000 -i 8

set -eu

here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/.." && pwd)

rebuild() {
    SAN_CFLAGS="-O0 -g3 -fno-omit-frame-pointer -fsanitize=address"
    SAN_LDFLAGS="-fsanitize=address"

    cd "$top"

    if [ ! -f configure ]; then
        if [ -x ./autogen.sh ]; then
            ./autogen.sh
        else
            autoreconf -fi
        fi
    fi

    # Wipe and reconfigure with sanitizer flags. distclean is best-effort:
    # if there's nothing to clean, ignore the error.
    make distclean >/dev/null 2>&1 || true

    ./configure CFLAGS="$SAN_CFLAGS" LDFLAGS="$SAN_LDFLAGS"
    make -j
}

run() {
    cd "$here"

    # ASan options:
    #   detect_leaks=1            -- LSan at exit
    #   abort_on_error=1          -- core dump on first error, easier to gdb
    #   strict_string_checks=1    -- catch out-of-bounds in str* funcs
    #   halt_on_error=1           -- stop the run on the first error
    ASAN_OPTIONS="detect_leaks=1:abort_on_error=1:strict_string_checks=1:halt_on_error=1"
    export ASAN_OPTIONS

    # Same libtool wrapper trap as valgrind: go through libtool --mode=execute
    # so the real ELF in stress/.libs/ is launched with LD_LIBRARY_PATH set.
    exec "$top/libtool" --mode=execute \
	 "$here/tapdisk-vbd-stress" \
	 -T 10 -l 0 \
	 "$@"
}

case "$1" in
--no-rebuild|--no-build)
    shift
    run "$@"
    ;;
--rebuild|--build)
    rebuild
    ;;
*)
    rebuild
    run "$@"
    ;;
esac


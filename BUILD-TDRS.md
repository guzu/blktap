# Building tapdisk with the tdrs driver

The `tdrs` driver (`drivers/block-tdrs.c`) serves qcow2 images (and their
backing chains) with the qcow2 implementation of blktap-rs, a Rust rewrite
of blktap. The Rust code comes as a static library, `libtdrs.a`, built from
a blktap-rs checkout and linked into `tapdisk`. Unlike `block-qcow2.c`
(qemu's qcow2, one IOThread per queue), the Rust code runs on the tapdisk IO
threads themselves: no thread hop per request.

An image is opened as `tdrs:<path>` (a qcow2 image), for instance
`tap-ctl create -a tdrs:/dev/VG/LV`. Online commit (`tap-ctl commit`,
`query`, `cancel`) is supported. The architecture is described in
`blktap-rs/crates/tdrs/README.md`.

## 1. Get blktap-rs at the top of this tree

```sh
cd blktap-tdrs                  # this tree
git clone <blktap-rs repository> blktap-rs
git -C blktap-rs checkout ae9af4d
```

The C driver calls the functions declared in
`blktap-rs/crates/tdrs/include/tdrs.h`: build with a blktap-rs commit that
matches this branch. This branch was built and tested with blktap-rs
`ae9af4d` (the online commit needs `869ad65` or later).

The checkout must be named `blktap-rs` and sit at the top of the tree: that
is where `./configure --with-tdrs` looks for it. It is ignored by git (the
`/blktap-*/` rule of `.gitignore`). Any other location works with
`--with-tdrs=<dir>`.

## 2. Build libtdrs

A Rust toolchain is needed (edition 2024: rustc 1.85 or later).

```sh
(cd blktap-rs && cargo build --release -p tdrs)
```

This produces `blktap-rs/target/release/libtdrs.a`. Only the `tdrs` package
is built: the tapdisk binary of blktap-rs, and its allocator (mimalloc), are
not needed.

## 3. Build blktap

```sh
./autogen.sh
./configure --with-tdrs [other options]
make
```

`configure` stops if `blktap-rs/crates/tdrs/include/tdrs.h` is missing.
`libtdrs.a` is linked into `tapdisk`, `td-util` and `tapdisk-stream`; check
with `nm drivers/.libs/tapdisk | grep ' T tdrs_'`. Without `--with-tdrs`,
nothing changes in the build.

After a change in blktap-rs, rebuild the library (step 2), then `make`:
the programs are linked again when `libtdrs.a` changes.

## On an XCP-ng 8.3 host

The system compiler (gcc 4.8) is too old for blktap: use devtoolset-11.
Build both on the host (glibc 2.17), with rustc installed (e.g. with rustup):

```sh
. /opt/rh/devtoolset-11/enable
(cd blktap-rs && cargo build --release -p tdrs)
./autogen.sh
CFLAGS="-O2 -g -Wno-stringop-truncation" ./configure --prefix=/opt/blktap-tdrs --with-tdrs
make -j8
make install DESTDIR=$PWD/stage    # then copy stage/opt/blktap-tdrs to /opt
```

`tap-ctl` of this build spawns the `tapdisk` of its own prefix
(`/opt/blktap-tdrs/libexec/tapdisk`). To serve the VMs (SM, xapi), install it
as the system blktap with symlinks, as `blktap-install.sh` does with
`/opt/blktap`, after saving the binaries in place:

```sh
ln -sf /opt/blktap-tdrs/sbin/tap-ctl /usr/sbin/tap-ctl
ln -sf /opt/blktap-tdrs/bin/tapback /usr/bin/tapback
ln -sf /opt/blktap-tdrs/libexec/tapdisk /usr/local/libexec/tapdisk
rm -f /usr/libexec/tapdisk
systemctl restart tapback
```

## Quick check

```sh
qemu-img create -f qcow2 /tmp/t.qcow2 1G
dev=$(tap-ctl create -a tdrs:/tmp/t.qcow2)    # e.g. /dev/xen/blktap-2/tapdev0
dd if=/dev/urandom of=/tmp/data bs=1M count=64
dd if=/tmp/data of=$dev bs=1M oflag=direct
cmp /tmp/data $dev && echo OK
tap-ctl list                                   # gives the pid and minor
tap-ctl destroy -p <pid> -m <minor>
qemu-img check /tmp/t.qcow2
```

The messages of libtdrs go to the syslog of tapdisk (`daemon.log` on
XCP-ng), under its tag: `grep tapdisk /var/log/daemon.log`.

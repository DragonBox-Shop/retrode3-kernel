# Retrode3 kernel sources and release policy

## Purpose and provenance

This tree tracks the Retrode3-specific kernel, not the generic upstream
Letux kernel alone.

* Linux base: `goldelico/letux-kernel`, branch `letux-7.3-rc5`,
  commit `f71a8243f157cac91094bc0f7106b7f51cfe5fd1`.
* Retrode3 module: driver changes integrated into
  `DragonBox-Shop/retrode3-kernel`,
  `kernel/letux-7.3-rc5-sms`,
  commit `8e271cf1f39c508bdef7350526e740c58bcde1c8`.
* Retrode3-specific Device Trees: restored from the previous Retrode3
  `master` (kernel 7.2) at
  `3b9e0712378bd6f93936d5b96645a3e63417f971`,
  then integrated with Letux's generic 7.3 LX16 DTS entry point
  and DTB build list.

This integration is a **source candidate**, NOT a verified byte-for-byte
reconstruction of Nikolaus's Oct 1 2026 Debian build
`7.3-rc5-20261001095807`. That build's exact Git revision,
full configuration and module-symbol data still need to be matched.

The currently installed device reports
`7.3.0-rc5-letux-mips-retrode3+` and the Retrode3 2.9.5 board
uses `lx16-retrode-2.9.5.dtb`. The ESP32 requires a newer firmware
for the Linux 7.3 setup; missing Wi-Fi functionality should not be
misattributed to a kernel or Device Tree regression without verification.
Firmware compatibility is tracked separately from the kernel source
reconstruction. Before declaring a kernel release hardware-approved,
validate the DTB, cartridge slots, GPIO and driver interfaces.

## Source / release branches

* `upstream/letux-current`: automated, force-updated mirror of
  `goldelico/letux-kernel:letux-current`. NEVER merge to released
  branches automatically.
* `develop`: reviewed work in progress, including hardware tests.
* `master`: release source, changed only with explicit approval after
  on-device testing and matching build provenance.
* `feature/*` and `integration/*`: isolated experimental work.
* `archive/*`: retained historical references.

The daily upstream workflow is maintained in
`.github/workflows/sync-letux-upstream.yml`.
GitHub executes scheduled workflows from the repository default
branch only (currently `project-retrode3-v2`). Before switching
that default to `master`, the same workflow MUST be included in
`master`. The mirror branch appears only after the first successful
run. The GitHub Actions job only fetches Letux and pushes the mirror ref.

## Verified Retrode3 2.9.5 DTB reproduction (2026-11-22)

The 2.9.5 Device Tree has been independently built from this integration
branch with the installed device's `/boot/FULLCONFIG` as starting
configuration (`make ARCH=mips olddefconfig` followed by
`make ARCH=mips dtbs`, using the local MIPS cross-toolchain).
The resulting `arch/mips/boot/dts/ingenic/lx16-retrode-2.9.5.dtb`
was compared with the original
`/boot/ingenic/lx16-retrode-2.9.5.dtb` copied from the Retrode3's
`linux-image-letux-retrode3` package version
`7.3-rc5-20261001095807`.

Both files have SHA-256:

```
4f75917c3c63501f25de6f24a8c5434caa06b33d1ae62bfad39c7abe57b79425
```

`cmp` confirms byte-for-byte identity. This verifies the 2.9.5 DTB
output, **not** the exact original source revision, the other hardware
revisions, the entire kernel image, or its modules. These require
separate release verification.

## Successful independent full build (2026-11-22; hardware test pending)

The Retrode3 7.3 integration worktree was built under a rootless Podman
container (Debian bookworm) with the independently downloaded Bootlin
MIPS32EL glibc Buildroot 2018.11 GCC 8.2.0 cross-toolchain, which is
located **outside** the container on the developer host at
`~/retrode3-timing/toolchains/mips32el--glibc--bleeding-edge-2018.11-1`.
The original kernel's `init/.main.o.cmd` confirmed that the full
kernel used this compiler path:

```
/build/toolchains/mips32el--glibc--bleeding-edge-2018.11-1/bin/mipsel-linux-gcc
```

The mount is important: mount `~/retrode3-timing` as `/build`,
then build in `/build/retrode3-7.3-integration`. Mounting only the
kernel worktree does **not** make the toolchain available. For example:

```sh
podman run --rm --userns=keep-id \
  -v "$HOME/retrode3-timing:/build" \
  -w /build/retrode3-7.3-integration \
  localhost/retrode3-mips-builder:uboot \
  sh -ec '
    PREFIX=/build/toolchains/mips32el--glibc--bleeding-edge-2018.11-1/bin/mipsel-linux-
    make ARCH=mips CROSS_COMPILE="$PREFIX" LOCALVERSION=+ -j2 uImage modules dtbs
  '
```

The `uboot` image is a local derivative of the existing builder image
with Debian `u-boot-tools` installed to supply `mkimage`. Without it,
the compressed U-Boot image target fails even after `vmlinux` and
modules compile. Explicit `LOCALVERSION=+` is required to produce the
installed kernel release string; do not rely on `scripts/setlocalversion`
inferring `+` from the checkout's Git state.

Observed successful build outputs:

* Kernel release: `7.3.0-rc5-letux-mips-retrode3+`.
* `vmlinux` built; `arch/mips/boot/uImage` symlinks to `uImage.gz`.
* U-Boot image header: MIPS Linux kernel, gzip compressed; data size
  **6,651,493 bytes**; load address `80010000`; entry point
  `80b07568`. The header timestamp is not a reproducibility guarantee.
* `drivers/misc/retrode3/retrode3.ko` vermagic:
  `7.3.0-rc5-letux-mips-retrode3+ preempt mod_unload modversions MIPS32_R2 32BIT`.
* `modules.order` lists **405** modules, and **405** `.ko` files exist.
* `make ... modules dtbs` completed successfully after the `uImage`
  target was built.
* The 2.9.5 DTB also matches Nikolaus's packaged DTB byte-for-byte
  (see the separate SHA-256 verification above).

**Not yet validated:** runtime boot, functional on-device hardware
tests, exact compatibility of all symbol CRCs with Nikolaus's
installed binaries, and identity of the generated kernel image with
the October release. Do not replace an installed kernel or module
without backups and a tested rollback path. Keep this branch separate
from release branches until hardware approval.

## Reproducing the Oct 2026 kernel configuration

On a Retrode3 with Nikolaus's installed Debian image:

```sh
dpkg-query -W linux-image-letux-retrode3 linux-modules-letux-retrode3
cp /boot/FULLCONFIG /tmp/retrode3-7.3-rc5-FULLCONFIG
cat /boot/kernel.release
```

Use the FULLCONFIG from the original package (and the correct cross
compiler) in a separate build directory or checked-out worktree. The
configuration, compiler version, local version and exact commit must
match before comparing images. Do not copy `Module.symvers` from a
different kernel build when CONFIG_MODVERSIONS is enabled.

For an already-prepared matching kernel tree, build **only** the
Retrode3 module with:

```sh
make ARCH=mips CROSS_COMPILE=<mipsel-toolchain-prefix> \
    M=drivers/misc/retrode3 modules
```

For a full image / Device Tree verification use the matching build
configuration and compile the kernel and `dtbs` in a separate build.
The Retrode3 2.9.5 target is
`arch/mips/boot/dts/ingenic/lx16-retrode-2.9.5.dtb`.
Compare the resulting DTB with the file in Nikolaus's Debian package
before asserting source equivalence.

## Upstreaming our changes

Keep Retrode3 driver/DT patches as focused commits relative to the
appropriate goldelico Letux branch. After local review and hardware
testing, submit PRs to Nikolaus's selected Retrode3 integration branch.
Do not propose a wholesale kernel import or auto-PR every experimental
commit. Even after upstream accepts patches, advancement of our
`master` remains an explicit manual release step.

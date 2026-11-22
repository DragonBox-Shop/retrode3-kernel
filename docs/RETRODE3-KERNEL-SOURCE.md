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
uses `lx16-retrode-2.9.5.dtb`. Wi-Fi is reported not to work in
7.3, despite working in 7.1. Do not mark a new image as hardware-approved
until that regression and the full DTB have been checked.

## Source / release branches

* `upstream/letux-current`: automated, force-updated mirror of
  `goldelico/letux-kernel:letux-current`. NEVER merge to released
  branches automatically.
* `develop`: reviewed work in progress, including hardware tests.
* `master`: release source, changed only with explicit approval after
  on-device testing and matching build provenance.
* `feature/*` and `integration/*`: isolated experimental work.
* `archive/*`: retained historical references.

The daily upstream workflow lives on this repository's default branch
`project-retrode3-v2`:
`.github/workflows/sync-letux-upstream.yml`.
The mirror branch appears only after that workflow's first successful
run. The GitHub Actions job only fetches Letux and pushes the mirror ref.

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

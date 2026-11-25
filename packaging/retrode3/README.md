# Retrode3 Linux kernel Debian packages

The authoritative MIPS32 R2 little-endian Retrode3 FULLCONFIG is
`arch/mips/configs/retrode3_full.config` in the SAME Git commit as the
kernel source, driver and X1600 board DTS.

`bash packaging/retrode3/build-debs.sh /absolute/output version`
crossbuilds `retrode3-linux-image` and `retrode3-linux-modules`
with `mipsel-linux-gnu-`. These scripts do not publish, install, flash
or edit a host boot partition. The two packages share one exact Debian
version. The kernel release, `CONFIG_MODVERSIONS`, MIPS32_R2, PREEMPT
and module vermagic are checked before packaging.

Payloads are staged under
`/usr/lib/retrode3/kernel-candidates/<debian-version>/`.
Only a trusted *offline image assembler* or a separately reviewed
rollback-aware activation manager may copy the selected uImage, DTB
and modules into the active boot/runtime locations.

The buildserver in `DragonBox-Shop/retrode3` selects the exact Git
commit and cross-compiler container image; it caches candidates by
source SHA and recipe/toolchain hashes. It is a read-only Git consumer
and may never push source code upstream.

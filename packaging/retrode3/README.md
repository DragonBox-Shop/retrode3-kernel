# Retrode3 Linux kernel Debian packages

The authoritative MIPS32 R2 little-endian Retrode3 FULLCONFIG is
`arch/mips/configs/retrode3_full.config` in the same source commit.

`bash packaging/retrode3/build-debs.sh /absolute/output version`
crossbuilds an exact-version kernel and modules pair. The build never
installs packages, flashes a disk or mounts a host boot partition.

Normal packages use the live-v1 layout. Modules are owned by dpkg under
/lib/modules/<unique-kernel-release>. The kernel image retains its
uImage/DTB payload under /usr/lib/retrode3/kernel-candidates/<version>
because FAT cannot support normal dpkg hardlink backup semantics.
On configure, the image package checks the exact configured modules
dependency and vermagic, mounts the Retrode3 FAT boot partition if needed,
copies and verifies the kernel and both DTB names, and requests a reboot.
There is no persistent rollback copy or automatic reboot, and U-Boot
and bootargs are untouched. A power loss while updating the FAT partition
can require reflashing the SD image. The previous running kernel must
not load the new release's modules; reboot after updating.

The offline image builder explicitly sets RETRODE3_OFFLINE_IMAGE=1
during package installation. Its assembler populates the FAT image
from the same packaged boot files without mounting physical storage.
Explicit 0~retrode3test+ versions remain staged-only test bundles.

The buildserver in DragonBox-Shop/retrode3 pins the source commit and
toolchain container and caches packages using their input fingerprints.
Hardware acceptance remains a separate release gate.

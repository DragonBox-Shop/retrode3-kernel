#!/bin/sh
# Install the packaged kernel onto the actual Retrode3 FAT boot filesystem.
# Invoked by dpkg only after the exact-version modules dependency is configured.
set -eu
[ "${1:-}" = configure ] || exit 0
die() { echo "Retrode3 kernel installation: $*" >&2; exit 1; }
version=$(dpkg-query -W -f='${Version}' retrode3-linux-image)
modules=$(dpkg-query -W -f='${db:Status-Status} ${Version}' retrode3-linux-modules)
[ "$modules" = "installed $version" ] || die "matching modules are not configured"
case "$version" in ""|*[!0-9A-Za-z.+:~_-]*) die "invalid package version";; esac
payload="/usr/lib/retrode3/kernel-candidates/$version"
krel=$(cat "$payload/kernel-release")
case "$krel" in ""|*[!0-9A-Za-z.+_-]*) die "invalid kernel release";; esac
case "$krel" in *+r3.*) ;; *) die "not an installable kernel release";; esac
ko="/lib/modules/$krel/kernel/drivers/misc/retrode3/retrode3.ko"
[ -s "$payload/boot/uImage" ] &&
  [ -s "$payload/boot/ingenic/lx16-retrode-2.9.5.dtb" ] &&
  [ -f "/lib/modules/$krel/modules.dep" ] && [ -s "$ko" ] ||
  die "incomplete kernel package pair"
actual=$(modinfo -F vermagic "$ko" | sed 's/[[:space:]]*$//')
[ "$actual" = "$krel preempt mod_unload modversions MIPS32_R2 32BIT" ] ||
  die "module ABI mismatch"
# The image builder installs identical packages in a disposable chroot;
# it assembles the FAT partition offline and must not mount any host disk.
if [ "${RETRODE3_OFFLINE_IMAGE:-0}" = 1 ]; then
  echo "Offline image: boot files will be installed by the image assembler"
  exit 0
fi
[ -b /dev/mmcblk0p1 ] && [ -b /dev/mmcblk0p2 ] ||
  die "expected Retrode3 SD partitions not present"
[ -d /boot ] && [ ! -L /boot ] || die "unsafe /boot mountpoint"
root_id=$(findmnt -rn -M / -o MAJ:MIN)
sd_id=$(lsblk -dnr -o MAJ:MIN /dev/mmcblk0p2)
[ -n "$root_id" ] && [ "$root_id" = "$sd_id" ] ||
  die "root filesystem is not the expected Retrode3 SD partition (actual <$root_id>, expected <$sd_id>)"
boot_id=$(lsblk -dnr -o MAJ:MIN /dev/mmcblk0p1)
[ -n "$boot_id" ] || die "cannot identify boot partition"
mounted_here=0
tmp_image=
tmp_dtb=
tmp_alias=
cleanup() {
  result=$?
  trap - EXIT
  for tmp in "$tmp_image" "$tmp_dtb" "$tmp_alias"; do
    [ -z "$tmp" ] || rm -f -- "$tmp" || result=1
  done
  if [ "$mounted_here" = 1 ]; then
    umount /boot || result=1
  fi
  exit "$result"
}
trap cleanup EXIT
if ! findmnt -rn -M /boot >/dev/null; then
  mount -t vfat -o rw,nosuid,nodev,noexec /dev/mmcblk0p1 /boot
  mounted_here=1
fi
[ "$(findmnt -rn -M /boot -o MAJ:MIN)" = "$boot_id" ] &&
  [ "$(findmnt -rn -M /boot -o FSTYPE)" = vfat ] ||
  die "/boot is not the expected FAT boot partition (expected device <$boot_id>)"
case ",$(findmnt -rn -M /boot -o OPTIONS)," in *,rw,*) ;; *) die "/boot is read-only";; esac
mkdir -p /boot/ingenic
# Prepare and verify all files before replacing any active boot file.
# No bootloader sector writes, no saveenv and no persistent rollback copies.
tmp_image=$(mktemp /boot/.retrode3-image.XXXXXX)
tmp_dtb=$(mktemp /boot/ingenic/.retrode3-dtb.XXXXXX)
tmp_alias=$(mktemp /boot/ingenic/.retrode3-alias.XXXXXX)
cp -- "$payload/boot/uImage" "$tmp_image"
cp -- "$payload/boot/ingenic/lx16-retrode-2.9.5.dtb" "$tmp_dtb"
cp -- "$tmp_dtb" "$tmp_alias"
cmp -s "$payload/boot/uImage" "$tmp_image" &&
  cmp -s "$payload/boot/ingenic/lx16-retrode-2.9.5.dtb" "$tmp_dtb" &&
  cmp -s "$tmp_dtb" "$tmp_alias" || die "boot file verification failed"
sync -f /boot
mv -f -- "$tmp_dtb" /boot/ingenic/lx16-retrode-2.9.5.dtb
mv -f -- "$tmp_alias" /boot/ingenic/lx16.dtb
mv -f -- "$tmp_image" /boot/uImage
sync -f /boot
if [ "$mounted_here" = 1 ]; then
  umount /boot
  mounted_here=0
fi
# /run is cleared at reboot; never reboot or reload a driver during dpkg.
if [ "$(uname -r)" != "$krel" ]; then
  touch /run/retrode3-kernel-reboot-required
fi
echo "Retrode3 kernel $krel installed; reboot to activate."

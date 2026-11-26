#!/usr/bin/env bash
# MIPS32EL kernel .deb candidates. Installs nothing; never touches /boot.
set -euo pipefail
[[ "$#" == 2 && "$1" == /* ]] || { echo "Usage: $0 /abs/out version" >&2; exit 2; }
out="$1"; version="$2"
[[ "$version" =~ ^[0-9][A-Za-z0-9.+:~_-]*$ ]] || exit 1
src="$(cd "$(dirname "$0")/../.." && pwd -P)"
config="$src/arch/mips/configs/retrode3_full.config"
test -s "$config"
test "$(mipsel-linux-gnu-gcc -dumpmachine)" = mipsel-linux-gnu
check_required_config() {
  local file="$1" option
  for option in CONFIG_CPU_LITTLE_ENDIAN=y CONFIG_CPU_MIPS32_R2=y CONFIG_MODVERSIONS=y CONFIG_RETRODE3=m CONFIG_PREEMPT=y CONFIG_MODULE_UNLOAD=y; do
    grep -Fxq "$option" "$file" || { echo "Missing $option in $file" >&2; exit 1; }
  done
}
check_required_config "$config"
mkdir -p "$out"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/build" "$tmp/modules"
cp "$config" "$tmp/build/.config"
kmake() { make -C "$src" O="$tmp/build" ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- LOCALVERSION=+ "$@"; }
kmake olddefconfig
check_required_config "$tmp/build/.config"
echo "=== Effective Kconfig differences ==="
python3 "$src/scripts/diffconfig" "$config" "$tmp/build/.config"
krel="$(kmake -s --no-print-directory kernelrelease | tail -n 1)"
[[ "$krel" =~ ^[A-Za-z0-9.+_-]+$ ]] || exit 1
kmake -j2 uImage modules dtbs
kmake INSTALL_MOD_PATH="$tmp/modules" modules_install
mods="$tmp/modules/lib/modules/$krel"
ko="$mods/kernel/drivers/misc/retrode3/retrode3.ko"
test -s "$ko"; test -f "$mods/modules.dep"
rm -f "$mods/build" "$mods/source"
actual="$(modinfo -F vermagic "$ko" | sed 's/[[:space:]]*$//')"
expected="$krel preempt mod_unload modversions MIPS32_R2 32BIT"
[[ "$actual" == "$expected" ]] || { echo "ABI mismatch: <$actual> != <$expected>" >&2; exit 1; }
prefix="usr/lib/retrode3/kernel-candidates/$version"
img="$tmp/image"; mod="$tmp/module"
mkdir -p "$img/DEBIAN" "$mod/DEBIAN"
mkdir -p "$img/$prefix/boot/ingenic" "$mod/$prefix/lib/modules/$krel"
cp -L "$tmp/build/arch/mips/boot/uImage" "$img/$prefix/boot/uImage"
cp "$tmp/build/arch/mips/boot/dts/ingenic/lx16-retrode-2.9.5.dtb" "$img/$prefix/boot/ingenic/"
cp "$tmp/build/.config" "$img/$prefix/boot/config-$krel"
printf '%s\n' "$krel" > "$img/$prefix/kernel-release"
cp -a "$mods/." "$mod/$prefix/lib/modules/$krel/"
for spec in "retrode3-linux-modules:$mod" "retrode3-linux-image:$img"; do
  name="${spec%%:*}"; root="${spec#*:}"
  depends="Depends: kmod"
  [[ "$name" == retrode3-linux-image ]] && depends="Depends: retrode3-linux-modules (= $version)"
  cat > "$root/DEBIAN/control" <<EOF
Package: $name
Version: $version
Architecture: mipsel
Section: kernel
Priority: optional
Maintainer: OpenPandora GmbH <info@openpandora.org>
$depends
Description: Retrode3 MIPS32EL $name candidate
 Staged only; no active kernel or bootloader is replaced on install.
EOF
  deb="$out/${name}_${version}_mipsel.deb"
  [[ ! -e "$deb" ]] || exit 1
  dpkg-deb --root-owner-group --build "$root" "$deb"
done
echo "Kernel .deb candidates: $krel, $version"

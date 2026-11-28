#!/usr/bin/env bash
# MIPS32EL kernel Debian packages. The build never installs or touches host /boot.
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
mkdir -p "$tmp/modules"
build="$tmp/build"
cache_entry=
driver_key=
base_key=
# The server mounts a private cache, serialized by its platform build lock.
# Never use this cache for isolated test bundles or an unspecified toolchain.
if [[ "$version" != 0~retrode3test+* && -n "${RETRODE3_KERNEL_BUILD_CACHE:-}" ]]; then
  cache="$RETRODE3_KERNEL_BUILD_CACHE"
  [[ -d "$cache" && ! -L "$cache" ]] || exit 1
  keys="$(python3 "$src/packaging/retrode3/kernel-build-cache.py" "$src" "${RETRODE3_BUILDER_IMAGE_ID:?}")"
  read -r base_key driver_key <<< "$keys"
  [[ "$base_key" =~ ^[0-9a-f]{64}$ && "$driver_key" =~ ^[0-9a-f]{64}$ ]] || exit 1
  cache_entry="$cache/$base_key"
  [[ ! -L "$cache_entry" ]] || exit 1
  if [[ -e "$cache_entry" && ! -f "$cache_entry/complete" ]]; then
    echo "Discarding incomplete kernel build cache: $base_key"
    rm -rf -- "$cache_entry"
  fi
  build="$cache_entry/build"
fi
# The ABI fingerprint, not the Debian package version, identifies a cached
# kernel. Driver-only updates keep that kernel release and symbol CRCs.
# Uncached builds retain unique releases; test bundles keep their old layout.
layout=live-v1
localversion="+r3.$(printf '%s' "$version" | sha256sum | cut -c1-12)"
[[ -z "$base_key" ]] || localversion="+r3.${base_key:0:16}"
if [[ "$version" == 0~retrode3test+* ]]; then
  layout=staged-test
  localversion=+
fi
kmake() { make -C "$src" O="$build" ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- LOCALVERSION="$localversion" "$@"; }
if [[ -n "$cache_entry" && -f "$cache_entry/complete" ]]; then
  echo "kernel ABI: CACHE HIT $base_key (no uImage, DTB or full module build)"
  (cd "$cache_entry" && sha256sum --check --strict --quiet checksums)
  krel="$(cat "$cache_entry/kernel-release")"
  [[ "$krel" =~ ^[A-Za-z0-9.+_-]+$ && "$krel" == *"$localversion" ]] || exit 1
  check_required_config "$build/.config"
  cp -a "$cache_entry/modules/." "$tmp/modules/"
  driver_dir="$cache_entry/drivers/$driver_key"
  if [[ -f "$driver_dir/complete" ]]; then
    echo "retrode3.ko: CACHE HIT $driver_key"
    (cd "$driver_dir" && sha256sum --check --strict --quiet checksums)
  else
    echo "retrode3.ko: CACHE MISS $driver_key, building ONLY the Retrode3 module"
    # Kbuild's external module modpost reads the cached kernel Module.symvers.
    # MO isolates its output from the source checkout and the kernel objects.
    test -s "$build/Module.symvers"
    test -s "$build/include/generated/utsrelease.h"
    mkdir -p "$tmp/driver" "$cache_entry/drivers"
    kmake -j2 M="$src/drivers/misc/retrode3" MO="$tmp/driver" modules
    test -s "$tmp/driver/retrode3.ko"
    [[ ! -s "$tmp/driver/Module.symvers" ]] || {
      echo "RetroDe3 module exports symbols; full kernel ABI rebuild required" >&2; exit 1;
    }
    # Revalidate base files after the external Kbuild invocation.
    (cd "$cache_entry" && sha256sum --check --strict --quiet checksums)
    [[ ! -L "$driver_dir" ]] || exit 1
    mkdir -p "$driver_dir"
    cp "$tmp/driver/retrode3.ko" "$driver_dir/retrode3.ko"
    (cd "$driver_dir" && sha256sum retrode3.ko > checksums)
    touch "$driver_dir/complete"
  fi
  cp "$driver_dir/retrode3.ko" "$tmp/modules/lib/modules/$krel/kernel/drivers/misc/retrode3/retrode3.ko"
  depmod -b "$tmp/modules" "$krel"
else
  echo "kernel ABI: CACHE MISS ${base_key:-uncached}, building full kernel"
  mkdir -p "$build"
  cp "$config" "$build/.config"
  kmake olddefconfig
  check_required_config "$build/.config"
  echo "=== Effective Kconfig differences ==="
  python3 "$src/scripts/diffconfig" "$config" "$build/.config"
  krel="$(kmake -s --no-print-directory kernelrelease | tail -n 1)"
  [[ "$krel" =~ ^[A-Za-z0-9.+_-]+$ ]] || exit 1
  kmake -j2 uImage modules dtbs
  kmake INSTALL_MOD_PATH="$tmp/modules" modules_install
  rm -f "$tmp/modules/lib/modules/$krel/build" "$tmp/modules/lib/modules/$krel/source"
  if [[ -n "$cache_entry" ]]; then
    driver_dir="$cache_entry/drivers/$driver_key"
    mkdir -p "$cache_entry/modules" "$driver_dir"
    cp -a "$tmp/modules/." "$cache_entry/modules/"
    cp "$tmp/modules/lib/modules/$krel/kernel/drivers/misc/retrode3/retrode3.ko" "$driver_dir/retrode3.ko"
    (cd "$driver_dir" && sha256sum retrode3.ko > checksums)
    touch "$driver_dir/complete"
    printf '%s\n' "$krel" > "$cache_entry/kernel-release"
    (cd "$cache_entry" &&
      { printf '%s\0' build/.config build/Module.symvers build/include/generated/utsrelease.h \
           build/arch/mips/boot/uImage build/arch/mips/boot/dts/ingenic/lx16-retrode-2.9.5.dtb kernel-release;
        find modules -type f -print0; } | sort -z | xargs -0 sha256sum > checksums)
    touch "$cache_entry/complete"
  fi
fi
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
module_prefix="lib/modules/$krel"
[[ "$layout" != staged-test ]] || module_prefix="$prefix/$module_prefix"
mkdir -p "$img/$prefix/boot/ingenic" "$mod/$module_prefix"
cp -L "$build/arch/mips/boot/uImage" "$img/$prefix/boot/uImage"
cp "$build/arch/mips/boot/dts/ingenic/lx16-retrode-2.9.5.dtb" "$img/$prefix/boot/ingenic/"
cp "$build/.config" "$img/$prefix/boot/config-$krel"
printf '%s\n' "$krel" > "$img/$prefix/kernel-release"
cp -a "$mods/." "$mod/$module_prefix/"
for spec in "retrode3-linux-modules:$mod" "retrode3-linux-image:$img"; do
  name="${spec%%:*}"; root="${spec#*:}"
  depends="Depends: kmod"
  if [[ "$name" == retrode3-linux-image ]]; then
    depends="Depends: retrode3-linux-modules (= $version)"
    if [[ "$layout" == live-v1 ]]; then
      depends="$depends, kmod, mount, util-linux, coreutils"
      install -m 755 "$src/packaging/retrode3/kernel-postinst.sh" "$root/DEBIAN/postinst"
      sh -n "$root/DEBIAN/postinst"
    fi
  fi
  description="Staged only; no active kernel or bootloader is replaced on install."
  if [[ "$layout" == live-v1 ]]; then
    description="Installs matching kernel or modules; reboot after installing the pair."
  fi
  cat > "$root/DEBIAN/control" <<EOF
Package: $name
Version: $version
Architecture: mipsel
Section: kernel
Priority: optional
Maintainer: OpenPandora GmbH <info@openpandora.org>
$depends
X-Retrode3-Kernel-Layout: $layout
Description: Retrode3 MIPS32EL $name
 $description
EOF
  deb="$out/${name}_${version}_mipsel.deb"
  [[ ! -e "$deb" ]] || exit 1
  dpkg-deb --root-owner-group --build "$root" "$deb"
done
echo "Kernel .debs ($layout): $krel, $version"

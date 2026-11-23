# MegaDrive CFI bench on Retrode3 (Letux 7.3) - experimental

**Branch:** `feature/md-flash-cfi-7.3` based on kernel `develop`,
including the merged GBx-0011 transport. The test adds an isolated
`ioctl` for the **native 16-bit MegaDrive slot only**. It is not a
write-cart release and is NOT used by ROM auto-detection.

## What the kernel can do

When expressly enabled by root, and only for a present MD cartridge with
no other occupied slot, it checks a reported 3.3 V supply, takes the
driver's select mutex, issues **exactly one CFI query command**
(`word 0x55 := 0x0098`), reads up to 128 x16 query words (low byte only),
checks `QRY`, then writes `0x00F0` to word 0 for reset.
Both write lanes pulse together for full 16-bit commands. All error paths
attempt reset if entry was attempted, then release data GPIOs and CS.

The diagnostic returns raw CFI data for userspace geometry parsing. The
CFI chip's manufacturer / device IDs are **not** read yet, and an
identified CFI chip does not prove the cartridge board topology.

**Deliberately absent:** chip/sector erase, flash word/page programming,
SRAM write, general-purpose bus writes, automatic Flash auto-probe,
SST/Intel/8-bit query entry or reset sequences, and web write enablement.
The existing GBx, SMS and normal ROM read/write paths are unchanged.

## Hard blocks

- Module parameter `flash_cfi_test` defaults to **0** and is writable
  only by root (sysfs mode 0600).
- The ioctl requires `CAP_SYS_RAWIO` and an explicit command number,
  with version 1 and zero flags.
- MD slot with 16-bit data, correct compatible, data GPIOs and control
  pins must be present. Physical cartridge-detect must be active.
- 3.3 V must already be selected, checked by exact GPIO direction 1,
  not a negative error result; the ioctl never changes voltage.
- A second populated cartridge blocks the operation.
- Active GBx bench (including sticky `gbx_active`), SMS mapper bench,
  legacy bridge bench or Sonic 3 FRAM mode blocks the operation.
- Driver select mutex is taken nonblocking; a busy bus returns `EBUSY`.
  The userspace caller must ALSO use the existing Retrode3
  `/run/lock/retrode3/bus.lock`. The driver mutex cannot replace it.

**CAUTION:** CFI entry is a **write bus cycle**, even though it should
not erase or program a matching AMD/Fujitsu NOR chip. **Do not** run the
diagnostic on ordinary ROM carts, unknown adapters, or a populated second
slot. Initial hardware testing must use a known, expendable DragonBox
Flash-cart with a verified backup. Be ready to unload the test module if
the flash does not leave query mode; reset success has to be verified
against actual hardware.

## Safe build and deployment

On the developer's Arch workstation use the already prepared Letux
kernel tree (`/home/michi/retrode3-timing/letux-7.3-rc5`) with the
existing `.config` and `Module.symvers`. Verify that this test branch
is present in that tree; a GitHub commit alone does not update it.
Use Podman image `localhost/retrode3-mips-builder:latest` with the
kernel source mounted as a volume. Build **only this module**:

```sh
make ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- M=drivers/misc/retrode3 modules
modinfo drivers/misc/retrode3/retrode3.ko | grep -E 'vermagic:|filename:'
```

Expected vermagic must match
`7.3.0-rc5-letux-mips-retrode3+ preempt mod_unload modversions MIPS32_R2 32BIT`.
Do not run a complete kernel build; preserve the existing
`Module.symvers`. Transfer the **test** `.ko` to a separate path on the
Retrode3 via SCP; never overwrite the installed module without a backup.
Reload an isolated test module with **all adapters/cartridges removed**,
then confirm the kernel log and restore the original if anything fails.
This operation has not yet been performed for these sources.

Build the optional MIPS userspace diagnostic from its source:

```sh
${CROSS_COMPILE}gcc -std=gnu11 -O2 -Wall -Wextra -Werror \
  -Idrivers/misc/retrode3 \
  drivers/misc/retrode3/md_flash_cfi_bench.c -o md_flash_cfi_bench
```

The `_IOWR` number is computed by the *target* system headers and must
not be hardcoded from x86. Run the tool as root only after the test module
is loaded, the exclusive shared bus lock is held, and the MD slot is
manually set to 3.3 V:

```sh
echo 1 > /sys/module/retrode3/parameters/flash_cfi_test
./md_flash_cfi_bench /dev/slot-md
echo 0 > /sys/module/retrode3/parameters/flash_cfi_test
```

If the `QRY` test fails or the CFI region sizes are inconsistent, stop:
never attempt a fallback 5-V probe, generic bus writes or a Flash erase.

## Next acceptance stages

1. Verify real waveforms and reset-to-array behavior with a known
   EN29LV320CB Flash-cart, at 3.3 V, under the logic analyzer.
2. Report `QRY`, command set, interface width, size, erase regions and
   manufacturer/device IDs and compare against the existing FlashkitMD
   reference implementation and EN29LV320C datasheet.
3. Only after the query path passes, design separate **bounded,
   command-set-specific** erase/write/verify operations with hardware
   lock ownership, cancellation recovery and a non-overridable
   fail-closed allowlist. Review against concurrent GBx changes.
4. Wire the kernel transport to the optional `core_flash_v1` userspace
   ABI and release it only after physical readback verification.

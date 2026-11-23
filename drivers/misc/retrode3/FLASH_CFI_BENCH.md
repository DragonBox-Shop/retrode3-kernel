# MegaDrive CFI bench on Retrode3 (Letux 7.3) - experimental

**Branch:** `feature/md-flash-cfi-7.3` based on kernel `develop`,
including the merged GBx-0011 transport. The test adds an isolated
`ioctl` for the **native 16-bit MegaDrive slot only**. It is not a
write-cart release and is NOT used by ROM auto-detection.

## What the kernel can do

When expressly enabled by root, and only for a present MD cartridge with
no other occupied slot, it checks that the native MD connector is set to
5 V, takes the
driver's select mutex, issues **exactly one CFI query command**
(`word 0x55 := 0x0098`), reads up to 128 x16 query words (low byte only),
checks the lower-byte `QRY` at word addresses 0x10..0x12, then writes
`0x00F0` to word 0 for reset. Reserved words 0x00..0x0f are not read;
the high byte of an x16 query word is ignored, not treated as an error.
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
- The 5-V power GPIO may be on a sleep-capable controller. Query it using
  `gpiod_get_value_cansleep()` in the process-context diagnostic,
  avoiding a gpiolib WARN_ON.
- 5 V must already be selected on the native MD connector (power GPIO
  output low). Both direction and logical level are checked; negative
  GPIO errors never count as a valid voltage. The ioctl never changes voltage.
  This tests the voltage *selection*, not the actual measured VCC.
  Native console-compatible Flash carts can contain 5-V Flash ICs or
  3.3-V Flash ICs fed by an onboard regulator. Never apply 5 V directly
  to a bare 3.3-V IC or non-console-compatible adapter.
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
manually set to 5 V **only for a verified original-MegaDrive-compatible
Flash cartridge**, with the other slots empty. Verify the actual supply
and signal levels on the hardware before enabling the bench command:

```sh
echo 5000 > /sys/class/retrode3/slot-md/vcc
cat /sys/class/retrode3/slot-md/vcc  # expected selection: 5000mV
(
  trap 'echo 0 > /sys/module/retrode3/parameters/flash_cfi_test' EXIT
  echo 1 > /sys/module/retrode3/parameters/flash_cfi_test
  flock -n /run/lock/retrode3/bus.lock ./md_flash_cfi_bench /dev/slot-md
)
dmesg | tail -n 30
```

If the `QRY` test fails or the CFI region sizes are inconsistent, stop:
inspect the new `CFI bench:` kernel diagnostics, including raw x16 words
at the QRY addresses when identification fails, then validate the bus
waveforms and board-level power. Do not attempt generic bus writes or
Flash erase; do not blindly retry at another voltage.

## Next acceptance stages

1. Verify real waveforms and reset-to-array behavior with a known
   console-compatible EN29LV320CB Flash-cart, 5 V at the MD connector
   and the correct regulated VCC at the Flash IC, under the logic analyzer.
2. Report `QRY`, command set, interface width, size, erase regions and
   manufacturer/device IDs and compare against the existing FlashkitMD
   reference implementation and EN29LV320C datasheet.
3. Only after the query path passes, design separate **bounded,
   command-set-specific** erase/write/verify operations with hardware
   lock ownership, cancellation recovery and a non-overridable
   fail-closed allowlist. Review against concurrent GBx changes.
4. Wire the kernel transport to the optional `core_flash_v1` userspace
   ABI and release it only after physical readback verification.

# X1600 GPIOA ROM bulk-read experiment

The X1600 bulk ROM data-read path is **enabled by default** after
successful MD, SNES, NES and SMS hardware feedback. It remains independently
configurable. See ROM_ADDRESS_TIMING_TEST.md for the additional native ROM
address and continuous SNES OE optimizations. The existing hardware-verified
~37 kB/s X1600 bulk Flash write transport is unchanged.

## Motivation and source evidence

The Retrode 2.9.5 device tree in
`arch/mips/boot/dts/ingenic/lx16+retrode3.dtsi` maps D0..D15 to
**GPIOA PA8..PA23**, in strict ascending order. The X1600
`drivers/pinctrl/pinctrl-ingenic.c` driver reads `GPIO_PIN` at
register offset 0x00, and its per-bit `get()` invokes a separate
`regmap_read()` on the same GPIO bank for every requested bit.

This change caches the parent pinctrl driver's regmap once at
Retrode3 probe, **only if** the GPIO bank is the X1600 controller,
the chip label is GPIOA, it has exactly 32 pins and all 16 data
GPIOs are active-high, on GPIO offsets 8..23 in the expected order.
The bus keeps NULL for unsupported hardware, which then uses the
unchanged per-bit GPIO implementation.

When `rom_read_bulk_gpio=1`, `get_word()` uses **one** GPIOA
input-register read and yields bits 8..23 as a 16-bit value.
`get_half(bus, 1)` returns PA8..15 (D0..D7), and
`get_half(bus, 0)` returns PA16..23 (D8..D15). The existing
`/OE` pulses, 1us settle delays, all per-slot address selection,
chip-select control, bus direction, SRAM/FRAM write functions,
flash status polling and flash program transport are unchanged.
The return type in `read_half()` is corrected to `int` so a
GPIO failure isn't silently truncated to an 8-bit data value.

## Toggle on target (default ON)

The **same test kernel module** can be used for A/B comparisons:

```sh
# First load the new, separately backed-up test module.
dmesg | grep 'X1600 GPIOA PA8..PA23 bulk ROM read available'

# Prior behavior for control dump:
echo 0 > /sys/module/retrode3/parameters/rom_read_bulk_gpio
cat /sys/module/retrode3/parameters/rom_read_bulk_gpio
# N

# Optimized behavior for matching second dump:
echo 1 > /sys/module/retrode3/parameters/rom_read_bulk_gpio
cat /sys/module/retrode3/parameters/rom_read_bulk_gpio
# Y

# To revert without reloading:
echo 0 > /sys/module/retrode3/parameters/rom_read_bulk_gpio
```

The optimized mode defaults to ON. The setting is root-writable (mode
0600) for regression comparisons and future protected Advanced/Debug settings.
It is not yet exposed through the Web UI or CLI. Toggle only between complete
operations under the shared cartridge bus lock.

## Hardware acceptance

1. Build **only** `drivers/misc/retrode3/retrode3.ko` from the
   prepared Letux 7.3 tree using the usual Arch Linux / Podman
   MIPS32-R2 little-endian build, matching `.config`,
   `Module.symvers` and running `vermagic`. Do **not** rebuild
   the full kernel. Do not overwrite a working installed module
   without a separate backup and test module copy.
2. Start with one known-good MegaDrive game cart: with mode 0 read
   a complete ROM and record size, SHA-256 or CRC32 and elapsed time;
   repeat with mode 1, then switch mode 0 and repeat. All three
   images must be byte-identical, preferably also equal to a known
   independent reference image.
3. Repeat on a real SNES cart (8-bit data lanes) and a real NES
   cart (PRG/CHR). Check any headers and mapper-specific expected
   fingerprint and full readback, not only the displayed game title.
4. Repeat on any SMS/legacy adapters and other currently supported
   configurations before treating it as generally validated.
5. Record speed separately for MD, SNES and NES. The previously
   observed Flash Verify reader was ~151.1 KiB/s, but this is only
   a rough comparison, not a promised performance increase.
6. Because Flash Verify may use the same normal ROM reader, do not
   accept fast Write+fast Verify alone as independent evidence. Verify
   a written spare Flash cart with mode **0** as well and compare the
   resulting full image.

The native MD/SNES address optimization is now a separate configurable
path, documented in ROM_ADDRESS_TIMING_TEST.md. It does not change NES
mapper control or Flash command timing.

## Deployment status

The original test module has been tested on real hardware as described
in ROM_ADDRESS_TIMING_TEST.md. The default-on follow-up requires a module-only
build and load smoke test; broader cartridge and independent Flash acceptance
checks above remain useful. GitHub userspace CI does not validate the MIPS
kernel module.

# Native ROM address and SNES timing experiments

Branch based on the tested buffer-cleanup candidate. All new flags default
OFF. No change to the OSCR core, userspace HAL, CLI, Web UI, GBA direction
handling, NES mapper timing, SMS transport, Flash command/status sequences,
or FRAM access. No changes to the pinctrl driver or device tree.

* rom_addr_bulk_gpio: native MD aligned word and SNES byte ROM reads only.
  Uses the same X1600 shared regmap as the data-read path. PA address bits
  0..7,24..31 and PB 12..17,19..20 are validated against all 24 descriptors
  at probe. Only changed address bits are written to PAT0 CLEAR/SET
  (bank stride 0x100, offsets 0x48/0x44). No GPIO directions changed.
  Unknown topology retains per-bit access. A failed update marks the address
  cache dirty so the next successful address set drives every address bit.
* rom_snes_oe_hold: SNES mode 0 only; /OE stays active during address
  increments, with original 1us delay unless the separate delay flag is on.
  /OE is inactive before user copy, error cleanup and slot deselection.
  Existing legacy bridge CE high-Z handling is retained.
* rom_snes_fast_delay: SNES mode 0 only; requests ndelay(500) instead of
  udelay(1). Actual delay depends on MIPS implementation and hardware.
  Not a claim that this timing is supported by every cartridge.

Hardware A/B test with known-good normal SNES and MD carts, one cartridge
at a time, no populated adapter in another slot. Record elapsed time,
full ROM SHA256/CRC32, and compare against independent known-good dumps.
Do not use an untested combination for saves/flash programming.

All flags are root writable. Between complete reads, with no operation
running, set rom_read_bulk_gpio=1 and the three new flags to 0.
Run baseline, then enable each new flag individually (others 0),
then combine only individually validated flags, then baseline again.
Keep Flash command timing, NES timing and legacy read paths unchanged.
Verify MegaDrive CRC with address acceleration off/on, and NES
CRC regression. If any mismatch, disable the corresponding flag.

The new module is source-only until built against the matching prepared
kernel and validated on real hardware. Do not overwrite installed module.

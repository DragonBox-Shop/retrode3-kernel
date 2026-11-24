# Native ROM address and SNES timing optimizations

Hardware feedback for test commit 15fe208b8376d5240bc4be3486eba9630391e6c5:
SNES reads approximately 10–15 times faster with all test options on,
with correct CRC. NES, SMS and MegaDrive reported working without issues.
Reducing SNES settling delay to 500 ns made no meaningful speed difference,
so that experiment and its parameter have been removed. Keep the original
1 us settling delay. The three retained flags now default ON; the final
default-on module still requires a build/load smoke test.

* rom_read_bulk_gpio: X1600 PA8..23 data-bank snapshot (see ROM_BULK_READ_TEST.md).
* rom_addr_bulk_gpio: native MD aligned word and SNES byte ROM reads only.
  Uses the same X1600 shared regmap as the data-read path. PA address bits
  0..7,24..31 and PB 12..17,19..20 are validated against all 24 descriptors
  at probe. Only changed address bits are written to PAT0 CLEAR/SET
  (bank stride 0x100, offsets 0x48/0x44). No GPIO directions changed.
  Unknown topology retains per-bit access. A failed update marks the address
  cache dirty so the next successful address set drives every address bit.
* rom_snes_oe_hold: SNES mode 0 only; /OE stays active during address
  increments, with the original 1 us settling delay after each address.
  /OE is inactive before user copy, error cleanup and slot deselection.
  Existing legacy bridge CE high-Z handling is retained.

All three parameters remain root-readable/writable (0600) in
/sys/module/retrode3/parameters/. Set each to 0 for the legacy path or 1
for the optimized path. Change settings only between completed cartridge
operations, under the shared userspace cartridge bus lock. Future protected
Advanced/Debug settings may expose this exact allowlist. Runtime sysfs
changes do not persist across module reloads unless applied again.

No changes to OSCR cores, userspace HAL, CLI, Web UI, GBx direction handling,
NES mapper timing, SMS transport, Flash command/status sequences or FRAM
access. The existing Flash bulk write parameter is unchanged.

For regression testing use known-good normal carts, one at a time,
compare full ROM CRC/SHA256 and elapsed time with an independent reference
and with the optimized paths disabled. Broader cartridge coverage,
GBx integration and independent Flash verification remain separate tests.
Build only the module against the matching prepared target kernel tree.
Do not overwrite the installed module before verifying the new build.

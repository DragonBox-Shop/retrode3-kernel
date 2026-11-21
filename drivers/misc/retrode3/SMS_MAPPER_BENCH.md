# Experimental generated Sega 16-KiB SMS mapper transport (kernel 7.3)

This feature branch is derived from
`feature/legacy-plugin-ce-rd-hiz-test-7.3`, not the unrelated older
7.1-based `develop`. **Do not merge into master or develop.**

The Sanni-derived ROM bank loop is **already automatically generated** in
`retrode3-oscr` from pinned upstream `src/cores/SMS.cpp`. No duplicate
banking algorithm belongs in this kernel driver. This patch provides only
a minimal bus primitive for `begin_sega_banked`, `select_page2`, and
`end_sega_banked`.

## Mode 22, gated by two load-time parameters

Both `legacy_bridge_hiz_test=1` and `sms_mapper_write_test=1` must be
specified when loading this kernel module. Default is 0 for both.
With either off, all SMS mapper writes still fail.

Only the MD slot and fixed D0-D7 lane may write; only **exactly one
byte** to the physical MD addresses corresponding to SMS CPU mapper
registers `$FFFC..$FFFF`. The A22 (/WR) and A23 (/RD) physical address
positions, A17 (/MC-F) RAM disable and CE/RD bridge are explicitly
controlled within one mutex-protected transaction.

Write values are restricted:
- SMS $FFFC = 0 (disable SRAM)
- SMS $FFFD = 0 (map page 0 to bank 0)
- SMS $FFFE = 1 (map page 1 to bank 1)
- SMS $FFFF = bank 0..63 (page 2 bank switch; cleanup bank 2)

The path checks **5 V** at the physical MD slot and that no other
cartridge occupies another slot. No other write mode or address is
allowed. The normal high-Z SNES/MD adapter read logic is unchanged.

**Electrical risk:** Correct RAM, data-direction, and /RD /WR /CE
timing cannot be proven from code or a successful ROM header probe.
A brief 5-V mapper write can corrupt data or damage hardware if pin
routing assumptions are wrong. **Do NOT use on actual cartridges until
a logic analyzer or oscilloscope has verified the signals**, particularly
the presence and polarity of MD A23 (/RD), the CE/RD bridge and shared
data direction. 3.3-V header probes require no mapper writes.

This branch is a code integration and mock/simulation target only until
separately accepted on hardware. Never install as the default module.

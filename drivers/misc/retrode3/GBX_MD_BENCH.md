# Retrode2 GBx-0011 MD-slot kernel bench (Letux 7.3)

Status: **manual GB and GBA ROM-header probes passed on real Retrode3 hardware at 3.3 V**. Full ROM dumps, MBC bank switching, GBC-specific behavior, electrical validation, and production integration remain pending.
Branch: `feature/gbx-md-read-7.3` on `DragonBox-Shop/retrode3-kernel`.
Stage-1 portable reader is in `retrode3-oscr` branch
`feature/gbx-retrode2-md-transport`.

## Verified physical header results (2026-11-23)

* GB/GBC transport mode 23: actual **PINBALL** cartridge returned the same
  valid Nintendo-logo/header bytes and GB header checksum on two independent
  consecutive `python3 tools/gbx-md-hwtest.py gb --raw` runs. Reported
  `title='PINBALL' type=01 ROM=01 RAM=00` (MBC1, 64 KiB ROM, no RAM).
* GBA transport mode 24: actual **SHININGFORCE** cartridge returned a valid
  Nintendo-logo sum and GBA header checksum on
  `python3 tools/gbx-md-hwtest.py gba --raw` at 3.3 V.
  Reported `title='SHININGFORCE' game_code=AF5P`; GBA header began
  `2E 00 00 EA 24 FF AE 51` and contained title bytes at `0x00A0`.
* GBA address-latch bug was traced to a uniform +1 mismatch between the
  GBx adapter's MD A-pin labels and the Retrode3 host `addr-gpios[]`
  indices. After correcting **both** data and control signal indexes
  (and releasing the IRQ/audio GPIO), the GBA header passes.
* **Not yet established:** complete and reproducible GBA ROM dumping,
  successful GB MBC bank switching, GB/GBA electrical oscilloscope checks,
  independent GB regression **after the final control-line remap**,
  GBC-only behavior, and normal system regression. Keep on feature branches.

## Intended physical test

Use an **empty MD connector when loading the experimental kernel module**.
With no GB/GBC/GBA cartridge inserted, run the matching OSCR tool
`python3 tools/gbx-md-hwtest.py arm`. It sets `gbx_md_test=1` and
performs a single empty GBA-mode bus read, which makes MD A19 high-Z.
**Setting the module parameter alone does not reconfigure A19.**
Only after this arm step insert the cartridge and run the selected header
probe with `python3 tools/gbx-md-hwtest.py gb` or `gba`.

The GBx-0011 may not actuate the physical MD cartridge-detect switch,
especially with GBA. The experimental kernel allows opening the MD
character device without an active sense pin ONLY while `gbx_md_test=1`.
The validated GB/GBA header is the cartridge-family detection, always
at 3.3 V. An invalid or missing header is a failed probe, not permission
to try 5 V. The bypass is not enabled for normal MD reads.

The kernel blocks ALL ordinary slot reads and writes while armed; after the first
GBx operation it also blocks them if the parameter is subsequently reset.
Do not run another core until the GBx adapter is removed and the module
has been reloaded. Save the original module and never replace it in-place.

The operator chooses GB/GBC or GBA for the first test. The probe for
**both** families runs at **3.3 V**. GB/GBC reads can be explicitly
switched to **5 V after identification**. GBA transport refuses any
voltage other than 3.3 V. The kernel itself does not change voltages:
the central CLI hardware-profile mechanism will own that in the final
dynamic plugins, just as for SMS-MD. No automatic 5-V probes.

## First physical GB cartridge result and follow-up fix

First GB/GBC `--raw` header read at 3.3 V: the 48 bytes in
`0x104..0x133` matched the standard Nintendo logo under the exact
transformation `observed = (expected << 1) & 0xff`. The first title
bytes `A0 92 9C 84 82 98 98` similarly become `PINBALL` when
shifted right one bit. This is **not** random ROM data or proof of
incorrect GB address/control timing.

The original schematic/netlist notes document MD `A8..A15` for the
GBx high/data connector pins, but the live read on the Retrode3
`addr-gpios[8..15]` yielded data shifted left one, with all bit 0s
clear. This **suggests** GB data D0..D7 are actually observed via
`addr-gpios[9..16]`. Do not silently edit the source netlist claim:
the source of the one-pin discrepancy still requires confirmation
against the physical adapter/host wiring.

Before this correction, GPIO A16 was configured as an OUTPUT; if it
carries GB D7 this is a potential electrical conflict. The updated
bench **leaves A16 INPUT for GB** and samples nine-to-sixteen; after **every** GBx read (including the empty-bus arm step) it releases all shared GPIOs A8..A16 to INPUT/Hi-Z before cartridge insertion or removal. GBA
upper-address bits are tentatively driven on nine-to-sixteen as the
same shared connector pins. Neither result is considered validated
until a newly compiled test module produces a **valid GB header**
and then a valid GBA header at 3.3 V. Do not re-run the old GB bench
build. No 5-V switching on invalid headers.

## Follow-up: isolated bit 7

Second GB/GBC header read (after switching the eight sampled GPIOs
from A8..A15 to A9..A16) returns the correct ASCII title `PINBALL`,
and valid low seven bits in the Nintendo logo, but *all bit 7 samples
are zero*. Header checksum was calculated `AC` vs sampled `2C`,
again exactly the missing high bit. That validates the low seven-bit
window, not the proposed eighth GPIO input (A16).

Add **mode 26** for a read-only GPIO snapshot: byte offsets equal
`GB_address * 2`, the two returned bytes form a little-endian
mask of GPIO input levels on **MD A7..A18** during the same GB
`/CS_ROM` + `/RD` cycle. All of those pins are temporarily INPUT/Hi-Z
while in GB mode; A19 audio/IRQ is also always input. No writes.

To collect the diagnostic at 3.3 V after loading the updated
experimental kernel:

```sh
python3 tools/gbx-md-hwtest.py arm
# Insert the already identified GB 'PINBALL' cartridge:
python3 tools/gbx-md-hwtest.py gb --pins
```

The user-space diagnostic prints several known Nintendo-logo bytes with
expected D7 values alongside the sampled GPIOs. If none of the input
GPIOs correlate with D7 we must revisit the physical wiring or input
buffer direction rather than fabricating bit 7 in software. The
checksum will still fail until the actual signal path is fixed.

## GBA all-FF header investigation

A real GBA cartridge now electrically activates the MD slot sense LED,
but mode 24 (GBA low ROM window) returned only `FF` throughout the first
192 bytes at 3.3 V. The sense LED is driven by the MD cartridge-detect
GPIO independently of any successful GBA header identification.

Original Sanni firmware `GameBoyAdvance::setROM()` waits **500 ms**
after preparing the ROM bus, then `readWord()` drives AD address,
asserts /CS_ROM, releases AD0..15, asserts /RD, samples 16-bit data,
and returns the controls idle. Our normal GBA cycle already follows
this order, but the all-FF result does not yet tell which phase fails.

Experimental **mode 27**, read-only and 3.3 V only, records four
16-bit `MD D0..D15` GPIO snapshots at one selected GBA word:
1. while the multiplexed address is being driven and /CS is inactive
2. address still driven, after /CS has been asserted
3. after releasing all 16 data GPIOs to input, before /RD
4. after /RD has been asserted and settled (should be ROM data)

Each trace record is exactly eight bytes, little-endian: four `u16`
samples. Seek offset `(GBA_byte_address / 2) * 8` in mode 27,
limited to the first 256 GBA words. The operation is read-only, no
/WR pulses, 3.3 V required. Diagnostic settle times are intentionally
longer than normal ROM reads and do not change normal modes 24/25.

In the OSCR GBx feature branch, the Python tester adds:

```sh
# Update both feature branches and load the newly compiled .ko
# with gbx_md_test=1, arm with empty slot, then insert a GBA cart:
python3 tools/gbx-md-hwtest.py gba --trace
```

The tester waits 500 ms before GBA reads, mirroring Sanni's initial
setup delay. The trace distinguishes correct address drive and input
release from a cartridge that does not drive the bus when /RD goes
low. Do not conclude a specific missing signal from FF values alone.

## Critical follow-up from GBA random-address trace (23 Nov 2026)

The 3.3-V GBA trace showed real ROM words (Nintendo logo), but
read requests `0x0000` and `0x00A0` returned different values on
repetition, consistent with the GBA ROM's automatic sequential address
counter advancing while new addresses were not latched. The sampled
data before/after the driver's supposed /RD assertion were identical.

A common cause fits **both** the previously successful GB experiment
and the GBA failure: the GB data bus was offset by **+1 in the host's
`addr-gpios[]` indexes** relative to the adapter's MD A-numbering.
The prior driver only shifted GB data pins, leaving the other control
pins uncorrected. If the same offset applies to ALL address pins:
- former `GPIO3` used for /CS_ROM was actually physical **/RD**;
- physical **/CS_ROM** was `GPIO4`, which was continuously driven LOW
  by the "unused pin" setup. GBA therefore never got a fresh
  address-latch edge and continued reading sequential words;
- physical **/WR** is `GPIO7`, previously set to INPUT during the
  GB diagnostic (must remain HIGH for ROM reads);
- physical **GB audio/GBA IRQ** is `GPIO20`, previously driven LOW
  as the supposed clock (must instead be INPUT/Hi-Z);
- physical clock is `GPIO21`, now driven to a defined LOW idle.

**STOP using any previously compiled GBx test module** until this
full control-line correction is compiled, loaded with the adapter and
cartridge removed, and verified. The new mapping is still a
**hypothesis inferred from electrical ROM data** and should be
checked against actual PCB/host wiring or with a logic analyzer.
Never change the general MD Device Tree to compensate; the shift
belongs to this adapter's bench mapping.

New experimental controls in `retrode3_gbx.c` are:
`/RD=3`, `/CS_ROM=4`, `/CS_RAM=6`, `/WR=7` kept
HIGH, `audio/IRQ=20` INPUT, `clock=21` LOW, and
`data/upper-address=9..16`. Mode 26's optional pin snapshot
now spans `GPIO8..GPIO18`, since GPIO7 must not be turned into
an input. All shared `GPIO8..GPIO20` lines are released after
each bus operation. Normal GB/GBA modes, voltages, cart writes
and other existing slot drivers are otherwise unchanged.

For the **next test**, remove the GBx adapter/cartridge before
unloading the old module; build and stage the new `.ko` under a
fresh name; with an empty adapter/slot, load it with
`gbx_md_test=1` and run `arm` before inserting a GBA cart. Start
with `python3 tools/gbx-md-hwtest.py gba --raw`, which remains at
3.3 V and reads only 192 bytes of the header. If this fails, avoid
full ROM dumping and inspect the new trace. Then reconfirm GB with
a separate header-only 3.3 V run after a clean arm step.

## Device ABI (private experimental offsets)

`pread` on `/dev/slot-md` with 64-bit offset
`((uint64_t)mode << 24) | byte_offset`:

| Mode | Meaning | Byte offsets |
|---|---|---|
| 23 | GB/GBC ROM read 8-bit | 0..0x7fff |
| 24 | GBA ROM read, low window | 0..0xffffff |
| 25 | GBA ROM read, high window | 0..0xffffff (physical +0x1000000) |
| 26 | GB read-only GPIO pin snapshot, diagnostic only at 3.3 V | 0..0xffff (2 bytes per GB ROM address; A8..A18 => bits 0..10) |
| 27 | GBA bus-stage trace, diagnostic only at 3.3 V | 0..0x7ff (8 bytes per GBA word, 4 x 16-bit samples) |

The two GBA windows cover 32 MiB. Returned GBA bytes use little-endian
word order, matching the stage-1 portable `gba_read16(byte_addr)`.

In these modes, MD D0..15 carry GB addresses or multiplexed GBA
addresses/data; MD A8..15 are GB data inputs or GBA upper address
outputs. MD A2/A3 are GBx /RD and /CS_ROM. MD A5 (GB reset/GBA
/CS_RAM) and A6 (/WR) stay high. MD A19 is high impedance, for the
cartridge audio/IRQ output. Clock A20 is kept at a defined low level
for the ROM header bench. No MBC/flash/save writes are provided.

The driver keeps A19 high impedance after the transaction. It does NOT
restore normal MD pin driving while the adapter could still be inserted.
After removing the adapter, reload the original module for normal use.

**Kernel build (Arch / Podman):** use the prepared Letux 7.3 MIPS tree
with matching .config and Module.symvers, bind-mounted into
`localhost/retrode3-mips-builder:latest`. Build ONLY
`make ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- M=drivers/misc/retrode3 modules`.
Check vermagic against `7.3.0-rc5-letux-mips-retrode3+`. Do not replace
the installed original before testing the module copy.

The stage-1 tool in the OSCR feature branch is the target for header
inspection. Compare with real GB/GBC and GBA cartridges. Only after
the physical header test should this mode be wired to the generic
v3 plugin + automatic 3.3-V probe. The present mode offsets are
temporary and not part of a stable userspace ABI.

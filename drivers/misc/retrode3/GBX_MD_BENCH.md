# Retrode2 GBx-0011 MD-slot kernel bench (Letux 7.3)

Status: **source-only**, hardware build and electrical/ROM tests pending.
Branch: \`feature/gbx-md-read-7.3\` on \`DragonBox-Shop/retrode3-kernel\`.
Stage-1 portable reader is in \`retrode3-oscr\` branch
\`feature/gbx-retrode2-md-transport\`.

## Intended physical test

Use an **empty MD connector when loading the experimental kernel module**.
Before inserting the GBx-0011 adapter, set the root-only module parameter
\`gbx_md_test=1\`, then insert the adapter/cartridge to test. The kernel
blocks ALL ordinary slot reads and writes while armed; after the first
GBx operation it also blocks them if the parameter is subsequently reset.
Do not run another core until the GBx adapter is removed and the module
has been reloaded. Save the original module and never replace it in-place.

The operator chooses GB/GBC or GBA for the first test. The probe for
**both** families runs at **3.3 V**. GB/GBC reads can be explicitly
switched to **5 V after identification**. GBA transport refuses any
voltage other than 3.3 V. The kernel itself does not change voltages:
the central CLI hardware-profile mechanism will own that in the final
dynamic plugins, just as for SMS-MD. No automatic 5-V probes.

## Device ABI (private experimental offsets)

\`pread\` on \`/dev/slot-md\` with 64-bit offset
\`((uint64_t)mode << 24) | byte_offset\`:

| Mode | Meaning | Byte offsets |
|---|---|---|
| 23 | GB/GBC ROM read 8-bit | 0..0x7fff |
| 24 | GBA ROM read, low window | 0..0xffffff |
| 25 | GBA ROM read, high window | 0..0xffffff (physical +0x1000000) |

The two GBA windows cover 32 MiB. Returned GBA bytes use little-endian
word order, matching the stage-1 portable \`gba_read16(byte_addr)\`.

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
\`localhost/retrode3-mips-builder:latest\`. Build ONLY
\`make ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- M=drivers/misc/retrode3 modules\`.
Check vermagic against \`7.3.0-rc5-letux-mips-retrode3+\`. Do not replace
the installed original before testing the module copy.

The stage-1 tool in the OSCR feature branch is the target for header
inspection. Compare with real GB/GBC and GBA cartridges. Only after
the physical header test should this mode be wired to the generic
v3 plugin + automatic 3.3-V probe. The present mode offsets are
temporary and not part of a stable userspace ABI.

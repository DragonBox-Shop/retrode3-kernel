# MegaDrive Flash kernel transport (Letux 7.3)

**Branch:** `feature/md-flash-cfi-7.3`. This is the normal native
MegaDrive slot Flash transport for the shared Retrode3 CLI and dynamic
MegaDrive core. No operator unlock parameter, root-only Flash helper,
separate lab mode, or special installation approval is necessary.

## Hardware results (2026-11-23)

On the real Retrode3, the DragonBox MegaDrive cartridge with an
**EN29LV320CB-70TIP** Flash chip correctly answered CFI **QRY**:

```text
CFI: QRY | primary command set: 0x0002 | interface: 0x0002 | capacity: 4194304 bytes
  region 0: 8 blocks x 8192 bytes @ 0x00000000
  region 1: 63 blocks x 65536 bytes @ 0x00010000
Geometry consistent; flash erase/program NOT enabled.
```

After CFI exit/reset, a normal ROM read at byte offset 0x100 returned
`SEGA GENESIS`. These results validate **CFI and reset-to-read mode**.
The earlier faulty active-low `/OE` direction was corrected; do not
reuse older test modules.

**Not yet hardware-verified:** manufacturer/device autoselect result,
erase, programming, physical timing margins and full ROM readback.
Implementation and CI compilation are NOT a substitute for those tests.

## Normal software path

```text
Web Write Cartridge (www-data)
  -> /usr/bin/retrode3-cli  (existing NDJSON job stream and bus lock)
  -> /usr/lib/retrode3/cores-available/md.so
  -> /dev/slot-md
  -> retrode3.ko
```

The kernel Flash ABI provides:

- `RETRODE3_FLASH_IOC_CFI_QUERY` for the AMD-style x16 CFI table
- `RETRODE3_FLASH_IOC_IDENTIFY` for CFI + manufacturer/device ID
- `RETRODE3_FLASH_IOC_ERASE` for one aligned sector
- `RETRODE3_FLASH_IOC_PROGRAM` for up to 512 bytes, even-sized and aligned

The **regular device-node permissions** decide which local processes may
use these operations, just as for ROM reads. The existing SysV Web
startup assigns the `retrode3-reader` group access to `/dev/slot-*`
and the MD voltage-control sysfs attribute.

**Compatibility checks that remain in the driver** are necessary for
the bus protocol: native MD x16 slot, present cartridge, 5000 mV selected
at the MegaDrive connector, no conflicting slot/bus activity, validated
CFI geometry, supported Eon EN29LV320CB bottom-boot ID, sector alignment
and legal program sizes. Unknown or unrecognized chips cannot be erased
or programmed. The driver resets the chip to read mode after commands.

The Flash core advertises `probe_flash`, `write_flash` and
`verify_flash`; Write Cart only becomes usable after successful Flash
identification. The CLI reports real `erase`, `program` and `verify`
progress. A write job only succeeds after byte-for-byte readback of the
selected ROM. Independent `verify_flash` performs no erase/program.

## MIPS module-only build

Use the existing Arch/Podman MIPS builder and the prepared
`/home/michi/retrode3-timing/letux-7.3-rc5` with its matching
`.config` and `Module.symvers`. Compile **only** the modified
`drivers/misc/retrode3/retrode3.ko`, not a complete kernel:

```sh
make ARCH=mips CROSS_COMPILE="<builder-toolchain-prefix>" \
  LOCALVERSION=+ M=drivers/misc/retrode3 -j2 modules
```

Verify `vermagic` matches
`7.3.0-rc5-letux-mips-retrode3+` and test the new `.ko` separately,
without overwriting the installed file during initial testing.

## Optional standalone operator diagnostic

`md_flash_rw_bench.c` remains available for low-level hardware
diagnostics but is not part of the normal customer application.
It takes `/dev/slot-md identify`, `write ROM.md`,
`erase ROM.md` and `verify ROM.md` with no extra confirmation
argument and no test parameter toggles. It requires an **external**
flock; the regular CLI already acquires that lock itself.

```sh
flock -n /run/lock/retrode3/bus.lock \
  /root/md_flash_rw_bench /dev/slot-md identify
```

Do not wrap the shared `retrode3-cli` in a second external flock.

The implementation remains on the feature branch until real-device
ID, erase/program, interruption and full Verify testing is complete.
That is a **release** decision, not a runtime function lock.

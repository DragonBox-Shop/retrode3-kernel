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

A sector erase waits for Flash status completion but does **not** scan
every word of the just-erased sector. Previously, each 64 KiB erase
performed another 32,768 slow GPIO reads, causing major delays.
The shared CLI offers independent Erase/Write/Verify stages, with
1..99 full-image Verify passes after writing. When Verify is disabled,
the result is `not_checked`. Programming still polls each word for
the Flash's command readiness.

In physical testing, erase stopped at 33/57 sectors with errno
`-16` (`EBUSY`). This indicates **bus contention or mode/slot state**,
not an erase/verify mismatch. The driver now waits for brief
`select_lock` contention and logs the conflicting GBx/SMS/bridge/FRAM
mode or another occupied slot if that causes EBUSY. Voltage selection
`3300mV` observed *after* the job may be the normal hardware-scope
restoration from the 5V write setting.

The Flash core advertises `probe_flash`, `write_flash` and
`verify_flash`; Write Cart only becomes usable after successful Flash
identification. The CLI reports real `erase`, `program` and `verify`
progress. A write job only succeeds after byte-for-byte readback of the
selected ROM. Independent `verify_flash` performs no erase/program.

## Program throughput: unlock-bypass optimization

A real cartridge's Erase completed quickly, but Write reached only
approximately 32 KiB per 10-15 seconds. The first implementation sent
two full unlock cycles, a program setup and a data word for **every**
16-bit word. Each bus cycle also configures and releases 16 Linux GPIO
data lines, so overhead dominates the Flash chip's typical 8 us internal
programming time.

The EN29LV320C supports **unlock-bypass** mode (AA/55/20 entry,
then A0 + word data for each program word, 90/00 exit). The program
ioctl now enters bypass only when an actual word needs writing, keeps
it for the bounded 512-byte chunk, and always exits before resetting
the chip to array-read mode. It retains the 1-to-0 bit transition check,
DQ6 completion polling, supported ID checks and optional full-image
Verify via the shared CLI.

This change is **not applied to the user's currently running Write**.
Build and install the updated `.ko` only after the ongoing job finishes
and the existing Verify result is known. The new throughput and bypass
exit behavior still require MIPS/kernel and cartridge tests. The
host-side syntax CI compiles only a userspace bench, **not the kernel**.

## Separate program status from full Verify; improve ROM-read copies

The first implementation did two different reads **inside every word
program operation**: an old-array-data read to test 1-to-0 programming,
and repeated array/status sampling to check command completion. This
was mistaken for a full Verify in the Write stage.

The old-array-data read and per-word content equality check are now
removed. The embedded program still **must finish** before the next
word command: the driver polls DQ6, checks DQ5 for timeouts and rechecks
DQ6 on a possible DQ5 race, following the vendor's status algorithm.
No complete ROM comparison runs in the Program stage. The fast word
poll no longer sleeps for 40-80 us each iteration. **Verify**, when
requested, still reads the entire image only after all programming
finishes, with the selected number of passes.

The ordinary MD-ROM `read()` path also supplies Flash readback and
Verify. It toggles /OE and samples 16 individual data GPIOs per word.
If `CONFIG_RETRODE3_BUFFER` was *off*, the old driver additionally did
one `copy_to_user()` for every 2-byte word. The new MD-ROM-only fallback
reads exactly the same bus words and performs the same /OE timing, but
copies a complete 1024-byte block at a time. When
`CONFIG_RETRODE3_BUFFER=y`, the existing buffered route is unchanged.
No SNES/NES/GBx/FRAM logic is touched.

Further GPIO bus-speed candidates are GPIO-chip `get_multiple` and
retaining data-direction configuration during adjacent Flash command
cycles. Both require explicit electrical and hardware throughput tests
before using them; the data GPIOs may span multiple SoC banks. The
512-byte program ioctl currently also re-identifies the chip before
each chunk, contributing extra CFI/autoselect overhead.

**Status:** this update requires a new module-only cross-build and
device tests. Host-side Flash bench CI alone does not validate the
kernel build, real throughput or GPIO read parity with prior ROM dumps.

## One Flash identification per open device session

The early implementation ran a complete CFI QRY + AMD autoselect
identity read **before every 512-byte program ioctl**, even though the
CLI had already identified the chip. This meant approximately 8192
redundant CFI/ID sequences for a 4 MiB image. The MegaDrive Flash
adapter also opened and closed `/dev/slot-md` on every 512-byte block.

The Flash adapter now keeps one file descriptor open from the
explicit Identify through Erase, Write and Verify. The kernel grants
erase/program permission only to the **same open file descriptor**
after a successful supported chip ID/CFI query. The authorization
is automatically discarded on close and invalidated when the
cartridge-detect polling worker sees a change. Each destructive
operation still independently checks the currently selected MD
slot, 5V setting, cartridge presence, exclusive kernel bus lock and
incompatible GBx/legacy/FRAM modes. A different opener without
its own successful Identify is rejected with ENODEV. No generic
raw bus write ioctl was added, and there is no persistent global
"Flash enabled" parameter.

The CFI query still runs for the **initial explicit Flash ID of
every job**, including erase-only. The user should not remove/replace
cartridges while a job is active. The 50ms detect polling period
does not guarantee protection against an unusually fast
remove/reinsert between two samples; there is no independent
physical cartridge identity signal for that case.

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

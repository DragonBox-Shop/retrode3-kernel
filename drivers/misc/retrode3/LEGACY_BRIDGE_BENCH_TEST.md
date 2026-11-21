# Legacy Retrode2 OE/CE bridge: temporary kernel bench test

**Feature branch only. Not production code. Not for merging to develop.**

## Hypothesis

SMS-0007, N64-0004 and GBx-0011 passive adapters tie MegaDrive /OE
(Retrode3 shared RD, GPB7 on board 2.9.5) to MegaDrive /CE
(MD CE, GPB28). This shorts two otherwise separately driven GPIO
outputs, even if the adapters are empty. A normal MD cartridge does not
make this connection. Disabling user-space cores cannot undo a passive
connection on the board.

## Opt-in behavior

Load `retrode3.ko` with `legacy_bridge_hiz_test=1`. Default `0`.
The parameter cannot be changed after module load.

- SNES `read(2)`: after selecting SNES, tri-state the MD CE GPIO
  (change from output to input). RD continues to pulse for SNES reads.
- MD fixed-lane adapter read (kernel read mode 20/21): prepare address
  and ensure RD inactive, acquire the selection lock, tri-state MD CE,
  pulse RD to read the adapter without also driving MD CE.
- End of read (including error paths): return RD to inactive, restore
  MD CE to output/inactive before releasing the slot selection lock.
- While enabled, reject normal MD/NES reads and **all cartridge writes**
  with `-EOPNOTSUPP`. Normal operations with parameter disabled are
  unchanged.

**CRITICAL LIMIT:** High-Z prevents two GPIO outputs fighting. It does
*not* stop RD from activating an adapter whose CE is tied to RD.
Therefore **SNES tests must use a COMPLETELY EMPTY adapter**, with
no SMS/N64/GB cartridge inserted. Two simultaneously populated slots
are not safe or supported by this experiment.

## Module-only build

Build against the **exact** configured, prepared kernel source and
Module.symvers that produced the running device kernel. From kernel
root, using the same CROSS_COMPILE/O= settings as your working build:

```sh
make ARCH=mips M=drivers/misc/retrode3 modules
modinfo drivers/misc/retrode3/retrode3.ko | grep -E 'filename:|vermagic:'
uname -r
```

With CONFIG_MODVERSIONS, the matching Module.symvers is required.
Do not run a whole-kernel build; do not install this test module under
/lib/modules.

## Bench procedure

1. With power **off**, insert Terranigma in SNES and an **empty**
   SMS-0007/N64-0004/GBx-0011 adapter in MD.
2. Stop the automatic Web reader:
   `service retrode3-web-v2 stop`. Close all manual CLI readers.
3. `modprobe -r retrode3` (stop if busy; do not force removal).
4. `insmod /absolute/path/to/test/retrode3.ko legacy_bridge_hiz_test=1`
5. Check `cat /sys/module/retrode3/parameters/legacy_bridge_hiz_test`
   returns `Y`.
6. Probe SNES three times:
   `/usr/bin/retrode3-cli --protocol ndjson-v1 --core snes --action probe`
7. Stop if abnormal behavior or dmesg errors occur. Collect:
   `dmesg | tail -n 70`.
8. Restore original module:
   `modprobe -r retrode3 && modprobe retrode3`.
   Verify parameter is absent or `N` before starting the Web backend
   via `service retrode3-web-v2 start`.

**Separate SMS test only:** Power off, remove the SNES cartridge, insert
an SMS cartridge in SMS-0007 and repeat steps 2-5. Probe ONLY
`--core sms-md --action probe` at 3.3 V. Do not run normal MD probe or
any ROM dump. Restore the original module afterward.

## Interpretation

SNES working with empty adapter in opt-in mode points to GPIO output
contention; failures indicate passive loading or other signals need
inspection. An improved SMS probe also points to contention during
the original fixed-lane read path. Neither proves simultaneous
populated-module operation is safe. Verify CE/RD timing electrically
before proposing any permanent solution. This parameter is temporary
and NOT a stable interface.

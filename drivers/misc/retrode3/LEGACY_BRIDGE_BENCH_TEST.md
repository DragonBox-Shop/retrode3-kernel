# Retrode2 legacy-adapter high-Z bench test, Letux 7.3-rc5

**Experimental branch only; not production code or a 7.1 -> 7.3 kernel upgrade.**

Based on the actual Letux 7.3-rc5 source in GitHub branch
`kernel/letux-7.3-rc5-sms` (commit `787a7e7d009af91edbfb759c5b8a66b06c16907f`). The SMS read-only
adapter lanes (20/21), address-before-select checks and protections are
preserved. These changes are not merged into 7.1 `develop` or `master`.

## Hypothesis

A passive Retrode2 SMS/N64/GBx adapter might bridge MD CE with a read
control signal. During a SNES read, MD CE as an inactive GPIO output
might oppose a pulsing RD output. This experiment places MD CE in
high-impedance input mode for SNES reads, and also during fixed-lane MD
adapter reads. This is a hypothesis, NOT a verified electrical diagnosis.

Load only for a short bench test using
`insmod retrode3.ko legacy_bridge_hiz_test=1` (default: off).
Normal MD/NES reads and ALL cartridge writes are blocked while enabled.
The GPIO direction is restored at read completion and failure exits.

**CRITICAL SAFETY:** High-Z MD CE does NOT isolate a second cartridge.
For SNES testing, the adapter in the MD slot must be **COMPLETELY EMPTY**.
Never test two populated cartridges simultaneously. Always power off
before inserting/removing adapters. Disable the automatic Web probe
and all other cart/sysfs accesses during this test.

## Build

Use the existing Arch Linux / Podman build with image
`localhost/retrode3-mips-builder:latest` and prepared source tree
`~/retrode3-timing/letux-7.3-rc5`, retaining its .config and
Module.symvers. Build ONLY `M=drivers/misc/retrode3 modules`, with
`ARCH=mips` and the working cross-compiler prefix. Verify:
`modinfo drivers/misc/retrode3/retrode3.ko | grep vermagic`.
Expected release: `7.3.0-rc5-letux-mips-retrode3+`.
Never overwrite the installed original module with this test module.

## Test

1. Power off. Insert SNES Terranigma and a completely empty SMS-0007
   adapter into MD. Later try empty N64-0004/GBx-0011 separately.
2. Power on; `service retrode3-web-v2 stop`. No other readers.
3. `modprobe -r retrode3` (never force removal).
4. `insmod /root/retrode3-bridge-test.ko legacy_bridge_hiz_test=1`.
5. Confirm `cat /sys/module/retrode3/parameters/legacy_bridge_hiz_test`
   prints `Y`.
6. Three SNES probes with
   `/usr/bin/retrode3-cli --protocol ndjson-v1 --core snes --action probe`.
7. Inspect `dmesg | tail -n 70` and stop on unusual behavior.
8. `modprobe -r retrode3 && modprobe retrode3`;
   `service retrode3-web-v2 start`.

For a **separate** SMS probe, remove the SNES cartridge after shutdown,
insert SMS game, use only `--core sms-md --action probe` at 3.3 V.
No regular MD probe and no writes or full ROM dump.

This test does not guarantee safety for any two-cartridge configuration.
Confirm actual wiring and control timing electrically before any
production change.

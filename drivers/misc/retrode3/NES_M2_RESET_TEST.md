# Native NES M2 soft reset experiment

Base: 8d8abdffea8c5c06cb945bec39daa85dee4fca38 (develop).
Status: source review only; not compiled or electrically qualified.

## Purpose

Mapper 60 increments an outer game counter on soft reset; mapper 233
toggles the outer ROM chip. Neither can be treated as a normal register
write or as a guaranteed power-cycle operation. The NES cartridge edge
does not carry CPU /RESET. A common RC circuit derives reset from an
interruption of M2 (NESdev, Implementing Mappers In Hardware).
On a console M2 is high impedance during reset; this experiment drives
M2 low instead. That is suitable for the documented diode/RC topology,
but not necessarily for every board.

## Interface

Native ioctl RETRODE3_NES_IOC_SOFT_RESET = _IO('R', 0xf4), arg=0, on
/dev/slot-nes opened O_RDWR. Do not copy an x86 ioctl number to MIPS.
The existing scoped profile mutex and slot selection lock are held.
Qualified X1600 NES GPIO topology only; busy/bench modes rejected.
No data outputs, no PRG/CHR write strobe, no supply change. M2 low
for at least 100 ms (scheduler can extend it), then existing idle
sequence restores high and waits 1-2 ms. No IRQ-off sleep.
Check cartridge presence before and after, plus observed cd_epoch.
On error attempt safe idle, deselect and return error, no retry.
The old flash ioctl dispatcher rejects unknown requests; never fall
back to arbitrary writes or a power cycle. No automatic mapper call.

## Hardware acceptance

* Build just drivers/misc/retrode3/retrode3.ko against the exact
  running kernel config and Module.symvers. Keep original installed
  module untouched. Load separate test module only after ABI check.
* Scope M2, /ROMSEL, CPU R/W, PPU /RD and /WR at cartridge connector.
  Verify low interval, high idle, all write strobes inactive,
  no unintended transitions, no power change; verify other slots
  isolated by existing selection circuit. Start without valuable carts.
* Test rejection on empty native slot, non-NES slot, read-only fd,
  nonzero argument, busy bus. Unknown ioctl on old module must fail.
* With known mapper 60 cart, capture the same PRG/CHR windows before
  and after each reset. Four distinct pairs then wrap; compare full
  dump CRC/order to reference. Two-game variant separately.
* With known mapper 233 cart, verify two chips toggle and wrap,
  determine initial chip using verified fingerprints. Do not infer
  canonical order from an arbitrary initial bank.
* Verify other NES mappers, MD and SNES remain unaffected during
  normal reads. No release or mapper enable until real validation.

References:
https://www.nesdev.org/wiki/Implementing_Mappers_In_Hardware
https://www.nesdev.org/wiki/INES_Mapper_060
https://www.nesdev.org/wiki/INES_Mapper_233

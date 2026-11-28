# NES explicit M2-pulse transport (development)
ABI: offset mode 17 PRG read / mapper write; 18 CHR read only;
19 mapper 59 address strobe write only. Native qualified X1600 NES slot
only. Read max 512 bytes, write exactly one byte, narrow address allowlist.
No ordinary-read fallback. Existing modes 10–16 unchanged.
Upstream reference e5a08326dfc8fff67193f7fc5a7bc36b5282b28c NES.cpp.
Compare M2, ROMSEL, CPU R/W, PPU /RD, data and address. Each pulse train
has eight low/high pairs (two groups for write); nominal half-period
250 ns plus MMIO overhead, unmeasured. IRQ disabled per active train,
not over entire block. M2 high between bytes/syscalls; check actual
inter-byte gaps. Kernel slot lock across each call. 59 no explicit M2
transition during active strobe. GPIO errors release strobes/data.
The electrical timing is NOT yet verified, nor is the module built
by this commit. Test with physical 57/226/237/289/314/332/59 boards.
Mapper 45 needs separate outer-register/CHR_M2 review.

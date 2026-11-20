Retrode3 passive cartridge adapter read modes
===============================================

Development-only ABI, NOT yet approved for physical SMS cartridge use.

Source/version status
---------------------

The `retrode3-kernel/develop` branch was based on
`project-retrode3-v2` (Linux 7.1). The actual Retrode3 device runs
Linux 7.3, with matching, complete sources still pending. Do NOT
replace or load a 7.1-built module on the running 7.3 system. These
changes have not been compiled or tested against that exact kernel.

Read-only offset modes
----------------------

The existing slot character-device ABI encodes a mode in the upper byte
of the file position, and the physical address in the lower 24 bits::

    file_offset = (mode << 24) | address

The following two modes are added:

.. list-table::
   :header-rows: 1

   * - Mode
     - Symbol
     - Allowed physical slot
     - Data lane
   * - 20
     - RETRODE3_ADAPTER_READ_D0_D7
     - MD (16-bit), SNES (8-bit)
     - D0..D7, regardless of A0
   * - 21
     - RETRODE3_ADAPTER_READ_D8_D15
     - MD (16-bit) only
     - D8..D15, regardless of A0

The driver uses its existing `read_half()` primitive to set an
address, pulse the common OE strobe, read the specified data lane and
release OE. Neither mode performs a write, sets power or voltage, or
changes the slot's configured bus width. Modes are rejected on NES or
slots with unexpected Device Tree compatibility/bus width; requests
crossing physical address width are rejected too. Writing via either
mode always fails with `-EOPNOTSUPP`. FRAM-active state is rejected.
Existing cartridge access modes are unchanged.

Limitations
-----------

These are generic low-level transport primitives, not adapter support.
A fixed 8-bit data lane plus common OE is not necessarily the same as
the Retrode2 Master System cartridge adapter's own RD/WR/OE sequencing.
Upstream Sanni SMS initialization uses 5 V and mapper writes. The
kernel must NOT execute those operations before verifying the electrical
pinout and timing of the particular Retrode2 adapter against the
Retrode3 2.9.5 PCB, and the actually running 7.3 driver.

An insertion/sense event proves occupancy only, NOT the adapter's
identity, compatible voltage, mapper or safe read path. Do not
hot-plug hardware during tests. The standard MegaDrive `MD_ROM`
mode is not a substitute for SMS cartridge read cycles.

This patch supplies no automatic adapter identification, bus power
policy, bank switching, writes to SMS registers, SRAM or save access.
The SNES adapter path still needs individual pinout and timing review.
Do not expose arbitrary physical addresses, register writes or voltages
through the web UI.

Hardware-free acceptance
------------------------

From the repository root::

    make -C tools/testing/selftests/retrode3 test
    make -C tools/testing/selftests/retrode3 clean

The test uses the exact shared C adapter-mode policy helper. It checks
the allowed physical slot widths and lane combinations. It is NOT a
kernel build or physical adapter test.

Crosscompile and hardware acceptance
------------------------------------

The proven Arch Linux crossbuild workflow uses the running target
configuration (FULLCONFIG), correct 7.3 kernel sources, and an
appropriate MIPS little-endian toolchain. The target previously used
mipsel-linux-gnu-gcc 8.2.0 with CONFIG_MODVERSIONS=y. A previous
GCC 12 attempt produced a mismatched devm_kmalloc symbol CRC.

Before attempting to load any rebuilt module, verify the target
`uname -r`, compiler, generated Module.symvers and `modinfo -F
vermagic`. A matching release string alone does not guarantee
symbol-version ABI compatibility.

Only after confirming the 7.3 sources, carry over the small, reviewed
change, crosscompile using the proven toolchain, run MD/SNES/NES
regression checks, and verify the adapter's pinout, supply and
control timing before controlled physical cartridge probing.

// SPDX-License-Identifier: GPL-2.0
/*
 * Retrode2 GBx-0011 on the MegaDrive socket.
 *
 * The adapter routes GB/GBA pins through the MD address/data GPIOs. Its
 * /RD and /CS are MD A2/A3, NOT the usual MD OE/CE controls. Do not use
 * the generic MD ROM reader. This bench transport does not issue any
 * mapper, cartridge RAM, save or flash write cycles.
 *
 * Explicit operator-only test modes. The caller sets 3.3 V for BOTH
 * header probes; a previously identified GB/GBC can be read at 5 V.
 * GBA is restricted to 3.3 V by the kernel.
 *
 * As long as the bench mode has been used, A19 remains an INPUT:
 * GB audio/GBA IRQ are cartridge outputs. The driver refuses all normal
 * bus access afterwards until the module is reloaded with the adapter
 * removed. This is intentionally conservative for the first hardware test.
 */

/*
 * GBx-0011 adapter signal GPIO indexes on the Retrode3 host.
 *
 * The schematic's MD A8..A15 GB data window was experimentally seen
 * on addr-gpios[9..16], one GPIO higher. A GB header became fully
 * valid after fixing that window. GBA data then followed the ROM's
 * sequential counter rather than honoring random addresses, strongly
 * suggesting that the same +1 discrepancy affects MD control pins:
 * former "CS" GPIO3 was the physical /RD, while physical /CS (GPIO4)
 * was previously held permanently low.
 *
 * Apply +1 to ALL MD address-side adapter pin indexes for this
 * operator-only bench. The shift is not yet electrically confirmed
 * against the adapter/host schematic; do not promote to production.
 */
#define GBX_HIGH_DATA_FIRST 9
#define GBX_HIGH_DATA_LAST  16
#define GBX_DIAG_FIRST      8
#define GBX_DIAG_LAST       18

/* GBx physical adapter pins, mapped onto Retrode3 addr-gpios indexes. */
#define GBX_RD        3  /* adapter MD A2, GBA ROM /RD */
#define GBX_CS_ROM    4  /* adapter MD A3, GBA address latch /CS */
#define GBX_CS_RAM    6  /* adapter MD A5, GBA RAM select / GB reset */
#define GBX_WR        7  /* adapter MD A6: ALWAYS HIGH (no writes) */
#define GBX_CLK      21  /* adapter MD A20, safe low clock */
#define GBX_AUDIO_IRQ 20 /* adapter MD A19, MUST be input */

static bool retrode3_gbx_md_slot(struct retrode3_slot *slot)
{
	return slot->bus_width == 16 &&
		of_device_is_compatible(slot->dev.of_node,
			"openpandora,retrode3-slot-megadrive");
}

/* Set the control lines INACTIVE before changing data-bus direction. */
static void retrode3_gbx_idle(struct retrode3_bus *bus)
{
	set_bus_bit(bus->addrs->desc[GBX_RD], 1);
	set_bus_bit(bus->addrs->desc[GBX_CS_ROM], 1);
	set_bus_bit(bus->addrs->desc[GBX_WR], 1);
	set_bus_bit(bus->addrs->desc[GBX_CS_RAM], 1);
}

/*
 * Each operation is serialized by the MD slot selection mutex. This
 * reconfigures the known MD edge pins; the ordinary MD address cache must
 * not be trusted after any GBx operation.
 */
static int retrode3_gbx_prepare(struct retrode3_bus *bus, bool gba)
{
	int i, ret;

	/* Disable the normal MD control strobes. */
	set_bus_bit(bus->oe, 1);
	gpiod_set_value(bus->we->desc[0], 0);
	gpiod_set_value(bus->we->desc[1], 0);
	gpiod_set_value(bus->time, 0);
	end_drive_word(bus);

	/* A19 is GB audio or GBA IRQ: never drive it in GBx mode. */
	ret = gpiod_direction_input(bus->addrs->desc[GBX_AUDIO_IRQ]);
	if (ret)
		return ret;
	bus->gbx_active = true;

	/* /RD, /CS, /WR and reset/CS_RAM: set inactive (HIGH). */
	ret = gpiod_direction_output(bus->addrs->desc[GBX_RD], 1);
	if (ret) return ret;
	ret = gpiod_direction_output(bus->addrs->desc[GBX_CS_ROM], 1);
	if (ret) return ret;
	ret = gpiod_direction_output(bus->addrs->desc[GBX_WR], 1);
	if (ret) return ret;
	ret = gpiod_direction_output(bus->addrs->desc[GBX_CS_RAM], 1);
	if (ret) return ret;

	/*
	 * Remaining unpopulated MD/N64-only pins can idle at 0.
	 * A8..A20 stay Hi-Z whenever not actively driving GBA
	 * upper addresses; A20 is the GB audio / GBA IRQ output.
	 */
	for (i = 0; i < bus->addrs->ndescs; ++i) {
		if (i == GBX_RD || i == GBX_CS_ROM ||
		    i == GBX_WR || i == GBX_CS_RAM ||
		    (i >= GBX_DIAG_FIRST && i <= GBX_AUDIO_IRQ))
			continue;
		ret = gpiod_direction_output(bus->addrs->desc[i], 0);
		if (ret)
			return ret;
	}
	/* Clock idles low for this read-only header/ROM transport. */
	set_bus_bit(bus->addrs->desc[GBX_CLK], 0);

	/*
	 * GB data bits 0..7 or GBA upper address bits 16..23 live
	 * on GPIO 9..16. Other GPIOs 8..20 remain inputs/Hi-Z.
	 */
	for (i = GBX_DIAG_FIRST; i <= GBX_AUDIO_IRQ; ++i) {
		if (i >= GBX_HIGH_DATA_FIRST && i <= GBX_HIGH_DATA_LAST &&
		    gba)
			ret = gpiod_direction_output(bus->addrs->desc[i], 0);
		else
			ret = gpiod_direction_input(bus->addrs->desc[i]);
		if (ret)
			return ret;
	}
	retrode3_gbx_idle(bus);
	bus->current_addr = EOF - 1; /* invalidate normal MD address cache */
	return 0;
}

/* Read eight GB data bits on the experimentally observed GPIO 9..16. */
static int retrode3_gbx_gb_read8(struct retrode3_bus *bus, u16 addr)
{
	unsigned int i;
	int value = 0, bit;

	set_word(bus, addr);             /* GB A0..A15 = MD D0..D15 */
	set_bus_bit(bus->addrs->desc[GBX_CS_ROM], 0);
	set_bus_bit(bus->addrs->desc[GBX_RD], 0);
	udelay(1);
	for (i = 0; i < 8; ++i) {
		bit = get_bus_bit(bus->addrs->desc[GBX_HIGH_DATA_FIRST + i]);
		if (bit < 0) {
			value = bit;
			break;
		}
		value |= !!bit << i;
	}
	retrode3_gbx_idle(bus);
	return value;
}


/*
 * Bench-only GB pin snapshot. For one ROM address, return bits 0..10
 * describing INPUT GPIOs addr-gpios A8..A18 while /RD is active.
 * The prior 12-bit A7..A18 diagnostic included physical /WR at GPIO7,
 * which must now remain driven HIGH at all times.
 * Do not expose this mode in a normal cartridge core / web interface.
 */
static int retrode3_gbx_gb_pins(struct retrode3_bus *bus, u16 addr)
{
	unsigned int i;
	int value = 0;

	set_word(bus, addr);
	set_bus_bit(bus->addrs->desc[GBX_CS_ROM], 0);
	set_bus_bit(bus->addrs->desc[GBX_RD], 0);
	udelay(2);
	for (i = GBX_DIAG_FIRST; i <= GBX_DIAG_LAST; ++i) {
		int bit = get_bus_bit(bus->addrs->desc[i]);

		if (bit < 0) {
			value = bit;
			break;
		}
		value |= !!bit << (i - GBX_DIAG_FIRST);
	}
	retrode3_gbx_idle(bus);
	return value;
}

/* GBA: address on MD D0..15 + tested A9..16 high window (hypothesis). */
static int retrode3_gbx_gba_read16(struct retrode3_bus *bus, u32 byte_addr)
{
	u32 word_addr = byte_addr >> 1;
	int i, value;

	for (i = 0; i < 8; ++i)
		set_bus_bit(bus->addrs->desc[GBX_HIGH_DATA_FIRST + i],
			    (word_addr >> (16 + i)) & 1);
	set_word(bus, (u16)word_addr);
	/* All 24 address bits must settle before /CS_ROM latches them. */
	udelay(2);
	set_bus_bit(bus->addrs->desc[GBX_CS_ROM], 0);
	udelay(2);
	end_drive_word(bus);
	set_bus_bit(bus->addrs->desc[GBX_RD], 0);
	udelay(1);
	value = get_word(bus);
	retrode3_gbx_idle(bus);
	return value;
}

/*
 * Read-only GBA timing diagnostic, independent of normal ROM output.
 * For each GBA word, capture four data GPIO states:
 *   0: multiplexed address driven, /CS high, /RD high
 *   1: address still driven, /CS low, /RD high
 *   2: all data GPIOs INPUT, /CS low, /RD high
 *   3: all data GPIOs INPUT, /CS low, /RD low (ROM should drive)
 *
 * This reproduces the OSCR readWord() strobes, with extra settle time
 * to make the input/output handoff observable. Never assert /WR.
 */
static int retrode3_gbx_gba_trace(struct retrode3_bus *bus,
				 u32 byte_addr, u16 samples[4])
{
	u32 word_addr = byte_addr >> 1;
	int i, value, ret = 0;

	for (i = 0; i < 8; ++i)
		set_bus_bit(bus->addrs->desc[GBX_HIGH_DATA_FIRST + i],
			    (word_addr >> (16 + i)) & 1);
	set_word(bus, (u16)word_addr);
	udelay(2);
	value = get_word(bus);
	if (value < 0) {
		ret = value;
		goto done;
	}
	samples[0] = (u16)value;

	set_bus_bit(bus->addrs->desc[GBX_CS_ROM], 0);
	udelay(2);
	value = get_word(bus);
	if (value < 0) {
		ret = value;
		goto done;
	}
	samples[1] = (u16)value;

	end_drive_word(bus);
	udelay(5);
	value = get_word(bus);
	if (value < 0) {
		ret = value;
		goto done;
	}
	samples[2] = (u16)value;

	set_bus_bit(bus->addrs->desc[GBX_RD], 0);
	udelay(10);
	value = get_word(bus);
	if (value < 0) {
		ret = value;
		goto done;
	}
	samples[3] = (u16)value;
done:
	retrode3_gbx_idle(bus);
	return ret;
}

/*
 * Character-device offset still has the existing 24-bit address window.
 * Mode 24 provides GBA bytes 0..16MiB-1, mode 25 the upper 16MiB.
 * All bytes are presented in cartridge-native little-endian order.
 */
static ssize_t retrode3_gbx_read(struct retrode3_slot *slot,
				char __user *buf, size_t count,
				loff_t *ppos, u32 mode)
{
	struct retrode3_bus *bus = slot->bus;
	const bool gba = mode == RETRODE3_GBX_MD_GBA_READ16_LO ||
			 mode == RETRODE3_GBX_MD_GBA_READ16_HI;
	const bool pins = mode == RETRODE3_GBX_MD_GB_PINSCAN;
	const bool trace = mode == RETRODE3_GBX_MD_GBA_TRACE;
	const u32 addr = (u32)(*ppos & 0xffffffULL);
	/* 256 GBA words; each 8-byte record contains four 16-bit stages. */
	const u32 limit = (gba ? 0x1000000U :
			   (trace ? 0x800U : (pins ? 0x10000U : 0x8000U)));
	u32 full_addr;
	u8 block[128];
	size_t completed = 0;
	int err, voltage;

	if (!gbx_md_test || !retrode3_gbx_md_slot(slot) ||
	    slot->fram_mode || sms_mapper_write_test ||
	    legacy_bridge_hiz_test)
		return -EOPNOTSUPP;
	if ((u64)addr + count > limit)
		return -ERANGE;
	if (!count)
		return 0;
	/* Diagnostics are single 8-byte records to avoid partial cycles. */
	if (trace && ((addr & 7U) || count != 8U))
		return -EINVAL;
	/* Bounded transactions prevent one large read from monopolizing the bus. */
	count = min_t(size_t, count, 4096);
	voltage = get_slot_power_mV(slot);
	if ((gba || pins || trace) && voltage != 3300)
		return -EPERM;
	if (!gba && voltage != 3300 && voltage != 5000)
		return -EPERM;
	full_addr = addr;
	if (mode == RETRODE3_GBX_MD_GBA_READ16_HI)
		full_addr |= 0x1000000U;

	/* The MD CE goes only to the unpopulated N64 pins on GBx-0011.
	 * Selecting MD here serializes against the regular slot reader. */
	select_slot(bus, slot);
	err = retrode3_gbx_prepare(bus, gba || trace);
	if (err)
		goto out;

	while (completed < count) {
		size_t i, n = min_t(size_t, sizeof(block), count - completed);

		retrode3_cart_led_activity(slot);
		for (i = 0; i < n; ) {
			if (trace) {
				u16 samples[4];
				unsigned int j;

				err = retrode3_gbx_gba_trace(bus,
						((full_addr + completed) >> 3) << 1,
						samples);
				if (err < 0)
					goto out;
				for (j = 0; j < 4; ++j) {
					block[i++] = (u8)samples[j];
					block[i++] = (u8)(samples[j] >> 8);
				}
			} else if (pins) {
				u32 offset = full_addr + completed + i;
				int snapshot = retrode3_gbx_gb_pins(bus,
								 (u16)(offset >> 1));

				if (snapshot < 0) {
					err = snapshot;
					goto out;
				}
				block[i++] = (offset & 1) ?
						(u8)(snapshot >> 8) :
						(u8)snapshot;
				if (!(offset & 1) && i < n)
					block[i++] = (u8)(snapshot >> 8);
			} else if (!gba) {
				err = retrode3_gbx_gb_read8(bus,
							   (u16)(full_addr + completed + i));
				if (err < 0)
					goto out;
				block[i++] = (u8)err;
			} else {
				u32 a = full_addr + completed + i;
				int word = retrode3_gbx_gba_read16(bus, a & ~1U);

				if (word < 0) {
					err = word;
					goto out;
				}
				block[i++] = (a & 1) ? (u8)(word >> 8) :
							  (u8)word;
				/* One physical GBA cycle returns both bytes. */
				if (!(a & 1) && i < n)
					block[i++] = (u8)(word >> 8);
			}
		}
		if (copy_to_user(buf + completed, block, n)) {
			err = -EFAULT;
			goto out;
		}
		completed += n;
		*ppos += n;
	}
	err = 0;
out:
	/*
	 * Leave ALL shared GB data/GBA upper-address lines and GPIO20
	 * (cart audio/IRQ) high-Z after every operation, including arm.
	 * Keep /WR inactive HIGH and do not restore normal MD address
	 * outputs while the GBx bench is armed.
	 */
	retrode3_gbx_idle(bus);
	end_drive_word(bus);
	{
		int pin;

		for (pin = GBX_DIAG_FIRST; pin <= GBX_AUDIO_IRQ; ++pin)
			gpiod_direction_input(bus->addrs->desc[pin]);
	}
	bus->current_addr = EOF - 1;
	select_slot(bus, NULL);
	return completed ? (ssize_t)completed : err;
}

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
 * MD-slot GPIO indices and GBx control lines (active LOW).
 *
 * First physical GB read: all 48 Nintendo-logo bytes were EXACTLY
 * (expected << 1) & 0xff. In this driver, sampling addr GPIOs 8..15
 * therefore returned GB D0 on bit 1 and never sampled GB D7.
 * The experimentally indicated 8-bit window is addr GPIOs 9..16.
 * This is a bench hypothesis, not a finalized hardware pinout.
 */
#define GBX_HIGH_DATA_FIRST 9
#define GBX_HIGH_DATA_LAST  16

/* MD edge address lines used as GBx control signals (active LOW). */
#define GBX_RD     2
#define GBX_CS_ROM 3
#define GBX_CS_RAM 5 /* GB reset, GBA RAM select: idle HIGH */
#define GBX_WR     6
#define GBX_CLK   20
#define GBX_AUDIO_IRQ 19

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

	/* The GBx adapter does not use the remaining MD/N64-only pins. */
	for (i = 0; i < bus->addrs->ndescs; ++i) {
		if (i == GBX_RD || i == GBX_CS_ROM ||
		    i == GBX_WR || i == GBX_CS_RAM ||
		    i == GBX_AUDIO_IRQ ||
		    (i >= 8 && i <= GBX_HIGH_DATA_LAST))
			continue;
		ret = gpiod_direction_output(bus->addrs->desc[i], 0);
		if (ret) return ret;
	}
	/* Clock idles low for this read-only header/ROM transport. */
	set_bus_bit(bus->addrs->desc[GBX_CLK], 0);

	/*
	 * Do not drive GPIO A16 as a normal MD address: on the GBx
	 * physical test the highest GB data bit appears to be here.
	 * A8 remains INPUT/Hi-Z. On GBA, A9..A16 drive the eight
	 * upper address bits; on GB the same GPIOs sample D0..D7.
	 */
	ret = gpiod_direction_input(bus->addrs->desc[8]);
	if (ret)
		return ret;
	for (i = GBX_HIGH_DATA_FIRST; i <= GBX_HIGH_DATA_LAST; ++i) {
		ret = gba ?
			gpiod_direction_output(bus->addrs->desc[i], 0) :
			gpiod_direction_input(bus->addrs->desc[i]);
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

/* GBA: address on MD D0..15 + tested A9..16 high window (hypothesis). */
static int retrode3_gbx_gba_read16(struct retrode3_bus *bus, u32 byte_addr)
{
	u32 word_addr = byte_addr >> 1;
	int i, value;

	for (i = 0; i < 8; ++i)
		set_bus_bit(bus->addrs->desc[GBX_HIGH_DATA_FIRST + i],
			    (word_addr >> (16 + i)) & 1);
	set_word(bus, (u16)word_addr);
	set_bus_bit(bus->addrs->desc[GBX_CS_ROM], 0);
	udelay(1);
	end_drive_word(bus);
	set_bus_bit(bus->addrs->desc[GBX_RD], 0);
	udelay(1);
	value = get_word(bus);
	retrode3_gbx_idle(bus);
	return value;
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
	const bool gba = mode != RETRODE3_GBX_MD_GB_READ8;
	const u32 addr = (u32)(*ppos & 0xffffffULL);
	const u32 limit = gba ? 0x1000000U : 0x8000U;
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
	/* Bounded transactions prevent one large read from monopolizing the bus. */
	count = min_t(size_t, count, 4096);
	voltage = get_slot_power_mV(slot);
	if (gba && voltage != 3300)
		return -EPERM;
	if (!gba && voltage != 3300 && voltage != 5000)
		return -EPERM;
	full_addr = addr;
	if (mode == RETRODE3_GBX_MD_GBA_READ16_HI)
		full_addr |= 0x1000000U;

	/* The MD CE goes only to the unpopulated N64 pins on GBx-0011.
	 * Selecting MD here serializes against the regular slot reader. */
	select_slot(bus, slot);
	err = retrode3_gbx_prepare(bus, gba);
	if (err)
		goto out;

	while (completed < count) {
		size_t i, n = min_t(size_t, sizeof(block), count - completed);

		retrode3_cart_led_activity(slot);
		for (i = 0; i < n; ) {
			if (!gba) {
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
	/* Keep A19 in high-Z, never restore a driven MD address bit
	 * while a GBx cartridge may still be present. */
	retrode3_gbx_idle(bus);
	end_drive_word(bus);
	bus->current_addr = EOF - 1;
	select_slot(bus, NULL);
	return completed ? (ssize_t)completed : err;
}

// SPDX-License-Identifier: GPL-2.0

/*
 * driver for Retrode 3 game cartridge driver
 *
 * based on ideas and fragments from
 *  drivers/char/mem.c
 *  drivers/gnss/core.c
 *  arch/arm/common/locomo.c
 *
 *  Copyright (C) 2022-26, H. Nikolaus Schaller
 *
 * FIXME: make this an independent driver module
 */

#include "retrode3_bus.h"
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/irqflags.h>

// #define RETRODE3_MAJOR FLOPPY_MAJOR
#define RETRODE3_MINORS	16
static DEFINE_IDA(retrode3_minors);
static dev_t retrode3_first;
static struct class *retrode3_class;

/*
 * game cart driver
 */

/* should depend on .compatible since it controls hardware peculiarities */

#define IS_MD()	(mode >= MD_ROM && mode <= MD_EEPMODE)
#define IS_SNES()	(mode == SNES_REGULAR || mode == SNES_HIROM)
#define IS_NES()	(mode >= NES_PRG && mode <= NES_WRAM)

/*
 * Adapter transport is deliberately narrower than a programmable bus.
 * Passive 8-bit reads may use a selected lane irrespective of A0;
 * physical connector/voltage/timing must still be validated separately.
 * Never expose this on the NES socket, whose high data lane is the PPU bus.
 */
static bool retrode3_adapter_read_allowed(struct retrode3_slot *slot,
						  u32 mode)
{
	bool md = of_device_is_compatible(slot->dev.of_node,
			"openpandora,retrode3-slot-megadrive");
	bool snes = of_device_is_compatible(slot->dev.of_node,
			"openpandora,retrode3-slot-snes");

	return retrode3_adapter_lane_allowed(mode, slot->bus_width, md, snes);
}

/* MegaDrive special gpio control */

#define TIME_HIGH (gpiod_set_value(slot->bus->time, 0))			// /TIME = low (active)
#define TIME_LOW (gpiod_set_value(slot->bus->time, 1))			// /TIME = high
#define MD_WE0_HIGH (gpiod_set_value(slot->bus->we->desc[0], 0))	// /WE0 = inactive (high)
#define MD_WE0_LOW (gpiod_set_value(slot->bus->we->desc[0], 1))		// /WE0 = active

/* SNES special gpio control */

// none yet

/* NES special gpio control */

// for NES address mapping: https://www.nesdev.org/wiki/CPU_memory_map

// can we simplify this? E.g. make the distinction between ROM (PRG) and RAM depend on address?
// so that /dev/slot mimicks the CPU_memory_map

/* Retrode gpios to NES special wiring
 d0..d7  <-> CPU (PRG = Program ROM/RAM - CPU = Central Processing Unit 6502)
 d8..d15 <-> PPU (CHR = Character ROM/RAM - PPU = Picture Processing Unit)
 a0..a14  -> A0..A14
 ce-nes & !a15 -> PRG/CE
 a16..a23 -> ignored
 romsel   -> A15
 ppu/a13  -> inverted A16
 unused   -> A17..A23
 we0      -> CPU_R/W
 rd / we8 -> PPU_/R/PPU_/W
 ce_nes   -> CPU_M2 (PHI2)
 for an example: https://www.nesdev.org/wiki/UxROM
*/

/* these macros map the OSCR software variables and functions to Retrode 3 kernel accessors */

/* 6502 bus timing
 * see Figure 6-3 of https://www.westerndesigncenter.com/wdc/documentation/w65c02s.pdf
 * or even better: https://www.princeton.edu/~mae412/HANDOUTS/timing.png
 *
 * Addr and R/W may change after PHI2_LOW
 * data can be read on PHI2 edge from L to H
 * write data is available some time after the rising edge
 *
 * this means:
 * we start assuming PHI2_LOW
 * we set up address and WE
 * we sample the data on read
 * then we issue PHI2_HI - this may remove ROMSEL and write badly?
 * if we write, we drive the bus
 * we do a PHI2_LO
 * we leave the bus driven or not as is until the next activity
 */

#define _delay_us(us) usleep_range(us, us)

#define PORTK_PRG(data) (set_half(slot->bus, data, 1))		// CPU/PRG D0..D7 := data (and output)
#define PORTK_CHR(data) (set_half(slot->bus, data, 0))		// PPU/CHR D8..D15 := data (and output)
#define PINK_PRG read_half(slot->bus, 1)
#define PINK_CHR read_half(slot->bus, 0)

// ROMSEL = PRG_/CE
static uint16_t romsel;
#define ROMSEL_HI { romsel = BIT(15); set_address(slot->bus->current_addr); }	// A15 = 1 makes ROMSEL 1
#define ROMSEL_LOW { romsel = 0; set_address(slot->bus->current_addr); }	// A15 = 0 makes ROMSEL 0
#define set_romsel(addr) { if ((addr) & BIT(15)) { ROMSEL_LOW ; } else { ROMSEL_HI; } }

// PHI2 = CPU_M2
/* on 6502 there is a PHI2_LOW at the start of every cycle and a PHI2_HI at the end and data sampling typically occurs on or after rising edge */
#define PHI2_HI (gpiod_set_value(slot->bus->phi2, 0))			// CPU_M2 = low
#define PHI2_LOW (gpiod_set_value(slot->bus->phi2, 1))			// CPU_M2 = high
// PRG = CPU = D0..D7 / CPU_R/W = WE0
#define PRG_READ (gpiod_set_value(slot->bus->we->desc[0], 0))	// /WE0 = inactive
#define PRG_WRITE (gpiod_set_value(slot->bus->we->desc[0], 1))	// /WE0 = active
// CHR = PPU = D8..D15 / PPU_R/W = WE8
// for OE/RD this is the pin level although we have "active low" in the device tree - reason unknown
#define CHR_READ_HI (gpiod_set_value(slot->bus->oe, 1), gpiod_set_value(slot->bus->we->desc[1], 0))	// RD = inactive, WE8 = inactive
#define CHR_READ_LOW (gpiod_set_value(slot->bus->oe, 0), gpiod_set_value(slot->bus->we->desc[1], 0))	// RD = active, WE8 = inactive
#define CHR_WRITE_HI (gpiod_set_value(slot->bus->oe, 1), gpiod_set_value(slot->bus->we->desc[1], 0))	// RD = inactive, WE8 = inactive
#define CHR_WRITE_LOW (gpiod_set_value(slot->bus->oe, 1), gpiod_set_value(slot->bus->we->desc[1], 1))	// RD = inactive, WE8 = active

#define MODE_READ (end_drive_word(slot->bus))			// D0..D15 = input
#define MODE_WRITE						// done automatically by set_half

/* CPU A15 is represented by /ROMSEL; PPU /A13 is on GPIO address 16.
 * Keep this translation shared by the legacy and bulk address paths.
 */
static inline u32 retrode3_nes_gpio_address(u32 addr)
{
	addr = (addr & 0x7fff) | romsel;
	if (!(addr & BIT(13)))
		addr |= BIT(16);
	return addr;
}

static inline int _set_address(int mode, struct retrode3_slot *slot, uint32_t addr)
{
	if (IS_NES())
		addr = retrode3_nes_gpio_address(addr);
// if ((addr &0xff) == 0) printk("%s: %08x\n", __func__, addr);
	return set_address(slot->bus, addr);
}

/* Read-only native NES path, after exact GPIO topology qualification. */
static inline int retrode3_nes_bulk_address(struct retrode3_slot *slot, u32 addr)
{
	return retrode3_set_rom_address(slot->bus,
				       retrode3_nes_gpio_address(addr), true);
}

#define set_address(addr) if ((err = _set_address(mode, slot, (addr))) < 0) goto failed

/*
 * Cartridge LED: steady when present, with brief inverted I/O pulses.
 * The LED core handles the blink; no GPIO/I2C calls in the byte loop.
 */
static inline void retrode3_cart_led_activity(struct retrode3_slot *slot)
{
	if (slot->cd_state == 1 && slot->status_led_trigger)
		led_trigger_blink_oneshot(slot->status_led_trigger, 90, 90, 1);
}

/*
 * This function reads the slot. The ppos points directly to the
 * memory location. But also includes a special access mode in the upper bits.
 */

/* this section needs cleanup!
 * it is a partial and modified and adapted copy of:
 *    https://github.com/sanni/cartreader/blob/0b8ac2ec14675ae55952ddf0b4590ba8d2899664/Cart_Reader/NES.ino#L1012
 */

/* Locate the MD connector by compatible, independent of slot ordering. */
static struct retrode3_slot *legacy_bridge_md_slot(struct retrode3_bus *bus)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(bus->slots); ++i) {
		struct retrode3_slot *candidate = bus->slots[i];

		if (candidate && of_device_is_compatible(candidate->dev.of_node,
				"openpandora,retrode3-slot-megadrive"))
			return candidate;
	}
	return NULL;
}

#include "retrode3_gbx.c"

/*
 * Explicit Sanni M2 modes; do not silently substitute ordinary PRG/CHR
 * cycles. X1600 MMIO topology only, no sleeping GPIO operations inside
 * pulse trains. The upstream AVR has implicit instruction-cycle delays;
 * 250 ns nominal half-period here is a starting point for measurement,
 * NOT a claim of electrically identical timing.
 */
static bool retrode3_nes_pulse_allowed(struct retrode3_slot *slot)
{
	struct retrode3_bus *bus = slot->bus;
	struct gpio_chip *gc, *ga;

	if (slot->bus_width != 8 || slot->fram_mode ||
	    !of_device_is_compatible(slot->dev.of_node,
				     "openpandora,retrode3-slot-nes") ||
	    !bus->rom_addr_bulk_available || !bus->rom_read_bank_map ||
	    IS_ENABLED(CONFIG_PREEMPT_RT) ||
	    IS_ERR_OR_NULL(bus->phi2) || gpiod_cansleep(bus->phi2) ||
	    !gpiod_is_active_low(bus->phi2) ||
	    IS_ERR_OR_NULL(bus->oe) || gpiod_cansleep(bus->oe) ||
	    !bus->we || bus->we->ndescs != 2 || bus->datas->ndescs != 16 ||
	    gpiod_cansleep(bus->we->desc[0]) ||
	    gpiod_cansleep(bus->we->desc[1]))
		return false;
	ga = bus->addrs->desc[15]->gdev->chip;
	gc = bus->phi2->gdev->chip;
	return gc && ga && gc->parent && gc->parent == ga->parent &&
	       gc->label && !strcmp(gc->label, "GPIOC") &&
	       gc->ngpio == 32 && gpiod_hwgpio(bus->phi2) == 26;
}

static int retrode3_nes_pulse_m2(struct retrode3_bus *bus, bool high)
{
	return regmap_write(bus->rom_read_bank_map,
		2 * R3_ROM_GPIO_BANK_STRIDE +
		(high ? R3_ROM_PAT0_SET : R3_ROM_PAT0_CLEAR), BIT(26));
}

static int retrode3_nes_pulse_train(struct retrode3_bus *bus)
{
	unsigned int i;
	int err;

	for (i = 0; i < 8; i++) {
		err = retrode3_nes_pulse_m2(bus, false);
		if (err)
			return err;
		ndelay(250);
		err = retrode3_nes_pulse_m2(bus, true);
		if (err)
			return err;
		ndelay(250);
	}
	return 0;
}

/* Return to an inactive CPU/PPU address before releasing data. Called
 * with the slot selected; never leave an asserted strobe on error. */
static int retrode3_nes_pulse_idle(struct retrode3_slot *slot)
{
	struct retrode3_bus *bus = slot->bus;
	int err = 0, ret;
	unsigned int d;

#define R3_PULSE_IDLE(op) do { ret = (op); if (!err && ret) err = ret; } while (0)
	romsel = BIT(15);
	R3_PULSE_IDLE(retrode3_nes_bulk_address(slot, 0));
	R3_PULSE_IDLE(set_bus_bit(bus->oe, 1));
	R3_PULSE_IDLE(gpiod_set_value(bus->we->desc[1], 0));
	R3_PULSE_IDLE(gpiod_set_value(bus->we->desc[0], 0));
	for (d = 0; d < bus->datas->ndescs; d++)
		R3_PULSE_IDLE(gpiod_direction_input(bus->datas->desc[d]));
	if (!err)
		R3_PULSE_IDLE(retrode3_nes_pulse_m2(bus, true));
#undef R3_PULSE_IDLE
	return err;
}

static ssize_t retrode3_nes_pulse_read(struct retrode3_slot *slot,
		char __user *buf, size_t count, loff_t *ppos, u32 mode, u32 addr)
{
	struct retrode3_bus *bus = slot->bus;
	bool prg = mode == NES_PRG_PULSE;
	bool single = mode == NES_CHR_M2;
	u8 data[512];
	size_t n;
	int err, value, ret;
	unsigned long flags;

	if (!retrode3_nes_pulse_allowed(slot))
		return -EOPNOTSUPP;
	if (!count)
		return 0;
	if (count > sizeof(data) ||
	    (prg ? (addr < 0x8000u || addr >= 0x10000u ||
		    count > 0x10000u - addr) :
		   (addr >= 0x2000u || count > 0x2000u - addr)))
		return -EINVAL;
	select_slot(bus, slot);
	err = gpiod_get_value(slot->cd);
	if (err <= 0) {
		err = err < 0 ? err : -ENODEV;
		goto out;
	}
	err = retrode3_nes_pulse_idle(slot);
	if (err)
		goto out;
	retrode3_cart_led_activity(slot);

	/* Complete each eight-pulse sample without IRQ/preemption gaps.
	 * M2 stays high between byte cycles, and between user transfers.
	 * Never hold IRQs off for an entire 512-byte block. */
	for (n = 0; n < count; n++) {
		local_irq_save(flags);
		/* Mapper 45 dumpCHR_M2: one low/high transition, then
		 * address and PPU /RD. Other pulse modes retain their
		 * preparatory address cycle and eight-pulse train.
		 * Data direction is prepared before the byte loop. */
		err = retrode3_nes_pulse_m2(bus, false);
		if (!err)
			err = retrode3_nes_pulse_m2(bus, true);
		if (!err && !single)
			err = retrode3_nes_pulse_m2(bus, false);
		romsel = BIT(15);
		if (!err)
			err = retrode3_nes_bulk_address(slot, addr + n);
		if (!err && !single)
			err = retrode3_nes_pulse_m2(bus, true);
		if (!err && prg) {
			romsel = 0;
			err = retrode3_nes_bulk_address(slot, addr + n);
		} else if (!err) {
			err = set_bus_bit(bus->oe, 0);
		}
		if (!err && !single)
			err = retrode3_nes_pulse_train(bus);
		if (!err && single)
			udelay(1);
		if (!err) {
			value = retrode3_bulk_rom_word(bus);
			if (value < 0)
				err = value;
			else
				data[n] = prg ? value & 0xff : (value >> 8) & 0xff;
		}
		if (!prg) {
			if (!single) {
				ret = retrode3_nes_pulse_m2(bus, false);
				if (!err) err = ret;
			}
			ret = set_bus_bit(bus->oe, 1);
			if (!err) err = ret;
			if (!single) {
				ret = retrode3_nes_pulse_m2(bus, true);
				if (!err) err = ret;
			}
		}
		local_irq_restore(flags);
		if (err)
			break;
	}
out:
	ret = retrode3_nes_pulse_idle(slot);
	if (!err) err = ret;
	select_slot(bus, NULL);
	if (err)
		return err;
	if (copy_to_user(buf, data, count))
		return -EFAULT;
	*ppos += count;
	return count;
}

static bool retrode3_nes_pulse_write_address(u32 mode, u32 addr, u8 data)
{
	if (mode == NES_ADDR_STROBE)
		return (addr >= 0x8000u && addr <= 0x8007u) ||
		       addr == 0x8020u || addr == 0x8040u || addr == 0x8060u;
	switch (addr) {
	case 0x8000: case 0x8800: case 0x6000: case 0x6001:
		return true; /* mapper-specific data checked by userspace */
	case 0x8001: return data <= 1u;
	case 0x5000: return data == 0x80u || (data <= 6u && !(data & 1u));
	case 0x5001: return data >= 0x80u && data <= 0xbfu;
	case 0x5002: return data < 16u;
	default: return false;
	}
}

static ssize_t retrode3_nes_pulse_write(struct retrode3_slot *slot,
		const char __user *buf, size_t count, loff_t *ppos, u32 mode, u32 addr)
{
	struct retrode3_bus *bus = slot->bus;
	bool strobe = mode == NES_ADDR_STROBE;
	unsigned long flags;
	unsigned int d;
	u8 byte;
	int err, ret;

	if (!retrode3_nes_pulse_allowed(slot))
		return -EOPNOTSUPP;
	if (count != 1)
		return -EINVAL;
	if (copy_from_user(&byte, buf, 1))
		return -EFAULT;
	if (!retrode3_nes_pulse_write_address(mode, addr, byte))
		return -EINVAL;
	select_slot(bus, slot);
	err = gpiod_get_value(slot->cd);
	if (err <= 0) {
		err = err < 0 ? err : -ENODEV;
		goto out;
	}
	err = retrode3_nes_pulse_idle(slot);
	if (err)
		goto out;
	/* Keep M2 high at deselected CPU $0000 during direction setup. */
	for (d = 0; d < 8; d++) {
		err = gpiod_direction_output(bus->datas->desc[d], (byte >> d) & 1);
		if (err)
			goto out;
	}
	local_irq_save(flags);
	/* The AVR prepares read/write direction over two M2 cycles
	 * before the address cycle. GPIO direction setup was moved to
	 * deselected $0000 above, to avoid a long M2-low phase. */
	if (!strobe) {
		err = retrode3_nes_pulse_m2(bus, false);
		if (!err) err = retrode3_nes_pulse_m2(bus, true);
		if (!err) err = retrode3_nes_pulse_m2(bus, false);
	}
	if (!err)
		err = gpiod_set_value(bus->we->desc[0], 1);
	if (!err && !strobe) {
		err = retrode3_nes_pulse_m2(bus, true);
		if (!err) err = retrode3_nes_pulse_m2(bus, false);
	}
	romsel = BIT(15);
	if (!err)
		err = retrode3_nes_bulk_address(slot, addr);
	if (!err && !strobe)
		err = retrode3_nes_pulse_m2(bus, true);
	if (!err && (addr & BIT(15))) {
		romsel = 0;
		err = retrode3_nes_bulk_address(slot, addr);
	}
	if (!err && strobe)
		udelay(1);
	if (!err && !strobe)
		err = retrode3_nes_pulse_train(bus);
	/* Upstream leaves ROMSEL selected during BOTH groups of eight. */
	if (!err && !strobe)
		err = retrode3_nes_pulse_train(bus);
	if (!strobe) {
		ret = retrode3_nes_pulse_m2(bus, false);
		if (!err) err = ret;
	}
	romsel = BIT(15);
	ret = retrode3_nes_bulk_address(slot, addr);
	if (!err) err = ret;
	ret = gpiod_set_value(bus->we->desc[0], 0);
	if (!err) err = ret;
	if (!err && !strobe) {
		err = retrode3_nes_pulse_m2(bus, true);
		if (!err) err = retrode3_nes_pulse_m2(bus, false);
	}
	ret = retrode3_nes_bulk_address(slot, 0);
	if (!err) err = ret;
	if (!err && !strobe)
		err = retrode3_nes_pulse_m2(bus, true);
	local_irq_restore(flags);
out:
	/* The address-only mode must not clock M2 on successful cleanup. */
	if (strobe && !err) {
		for (d = 0; d < bus->datas->ndescs; d++) {
			ret = gpiod_direction_input(bus->datas->desc[d]);
			if (!err) err = ret;
		}
	} else {
		ret = retrode3_nes_pulse_idle(slot);
		if (!err) err = ret;
	}
	select_slot(bus, NULL);
	if (err)
		return err;
	*ppos += 1;
	return 1;
}

static ssize_t retrode3_read(struct file *file, char __user *buf,
			size_t count, loff_t *ppos)
{
	struct retrode3_file_context *ctx = file->private_data;
	struct retrode3_slot *slot = ctx->slot;
	struct gpio_desc *bridge_md_ce = NULL;
	bool is_md = of_device_is_compatible(slot->dev.of_node,
			"openpandora,retrode3-slot-megadrive");
	bool is_snes = of_device_is_compatible(slot->dev.of_node,
			"openpandora,retrode3-slot-snes");
	uint32_t addr = *ppos & 0xffffff;	/* max address is 24 bit */
	uint32_t mode = *ppos >> 24;		/* access mode control */
	int err;
	/* Snapshot the ROM optimization controls once per read syscall. */
	bool native_rom = mode == MD_ROM && !slot->fram_mode &&
		((is_snes && slot->bus_width == 8) ||
		 (is_md && slot->bus_width == 16));
	bool bulk_address = native_rom && slot->bus->rom_addr_bulk_available &&
		READ_ONCE(r3_rom_addr_bulk_gpio);
	bool hold_oe = native_rom && is_snes &&
		READ_ONCE(r3_rom_snes_oe_hold);
	/* Only plain native NES ROM reads, never CHR_M2, RAM or register modes.
	 * The existing bulk-address switch also provides the full legacy
	 * NES fallback on this topology for hardware comparison.
	 */
	bool fast_nes = of_device_is_compatible(slot->dev.of_node,
			"openpandora,retrode3-slot-nes") &&
		slot->bus_width == 8 && !slot->fram_mode &&
		slot->bus->rom_addr_bulk_available &&
		READ_ONCE(r3_rom_addr_bulk_gpio) &&
		((mode == NES_PRG && addr >= 0x8000u && addr < 0x10000u &&
		  count <= 0x10000u - addr) ||
		 (mode == NES_CHR && addr < 0x2000u &&
		  count <= 0x2000u - addr));
	bool oe_held = false;

	ssize_t read;
	/* Small common buffer: avoid a 1 KiB stack frame and per-byte user copies. */
	u8 buffer[512];
	size_t fill = 0;

	read = 0;

	if (retrode3_gbx_mode_is_read(mode)) {
		if (!ctx->gbx_profile)
			return -EBUSY;
		return retrode3_gbx_read(slot, buf, count, ppos, mode);
	}
	/* Only the scoped GBx transport may use its alternate GPIO directions. */
	if (gbx_md_test || slot->bus->gbx_active)
		return -EBUSY;

	/* This bench mode deliberately blocks normal MD/NES cartridge reads. */
	if (legacy_bridge_hiz_test &&
	    !is_snes && !(is_md && retrode3_adapter_mode_is_read(mode)))
		return -EOPNOTSUPP;
	if (mode == NES_PRG_PULSE || mode == NES_CHR_PULSE ||
	    (mode == NES_CHR_M2 && count <= 512 &&
	     retrode3_nes_pulse_allowed(slot)))
		return retrode3_nes_pulse_read(slot, buf, count, ppos, mode, addr);
	if (mode == NES_ADDR_STROBE || mode == NES_REG)
		return -EOPNOTSUPP;

	/*
	 * Keep adapter access read-only, allowlisted by physical slot, and
	 * bounded to the slot's address pins before selecting any hardware.
	 * This mode neither selects a voltage nor changes data direction.
	 */
	if (retrode3_adapter_mode_is_read(mode)) {
		if (!retrode3_adapter_read_allowed(slot, mode))
			return -EOPNOTSUPP;
		/* Sonic 3 FRAM state never belongs to passive adapters. */
		if (slot->fram_mode)
			return -EBUSY;
		if (!slot->addr_width || slot->addr_width > 24 ||
		    (u64)addr + count > (1ULL << slot->addr_width))
			return -ERANGE;
	}

	if (retrode3_adapter_mode_is_read(mode)) {
		/* Do not even select an adapter after the cartridge was removed. */
		err = gpiod_get_value(slot->cd);
		if (err <= 0)
			return err < 0 ? err : -ENODEV;
		/*
		 * SMS-0007 on the MD socket maps A21 to /WR and A22 to /RD.
		 * Do NOT select /CE before the caller's full address (including
		 * the inactive /WR level) has been driven on the GPIO bus.
		 * Keep OE inactive, the data bus released and the kernel WE/TIME
		 * lines inactive until after the address has settled.
		 * This applies to BOTH adapter lanes and leaves legacy modes alone.
		 */
		select_slot(slot->bus, NULL);
		set_bus_bit(slot->bus->oe, 1);	/* physical OE inactive */
		gpiod_set_value(slot->bus->we->desc[0], 0);
		gpiod_set_value(slot->bus->we->desc[1], 0);
		gpiod_set_value(slot->bus->time, 0);
		end_drive_word(slot->bus);
		err = _set_address(mode, slot, addr);
		if (err < 0)
			return err;
		if (count) {
			if (legacy_bridge_hiz_test && is_md) {
				/*
				 * An empty legacy adapter may bridge RD and CE.
				 * Keep RD inactive while MD CE changes direction.
				 */
				set_bus_bit(slot->bus->oe, 1);
				mutex_lock(&slot->bus->select_lock);
				err = gpiod_direction_input(slot->ce);
				if (err < 0) {
					mutex_unlock(&slot->bus->select_lock);
					return err;
				}
				bridge_md_ce = slot->ce;
			} else {
				select_slot(slot->bus, slot);
			}
		}
	} else if (slot->bus_width != 16 || !slot->fram_mode) {
		select_slot(slot->bus, slot);	// legacy slot activation
		if (legacy_bridge_hiz_test && is_snes && count) {
			struct retrode3_slot *md = legacy_bridge_md_slot(slot->bus);

			if (!md || IS_ERR_OR_NULL(md->ce)) {
				err = -ENODEV;
				goto failed;
			}
			/*
			 * Only an EMPTY old adapter may occupy the MD slot.
			 * High-Z does NOT isolate an inserted cartridge.
			 */
			set_bus_bit(slot->bus->oe, 1);
			err = gpiod_direction_input(md->ce);
			if (err < 0)
				goto failed;
			bridge_md_ce = md->ce;
		}
	}

	if (addr + count >= EOF) {
		if (addr >= EOF)
			count = 0;	// read nothing
		else
			count = EOF - addr;	// limit to EOF
	}

// if ((addr &0xff) == 0)
// 	dev_info(&slot->dev, "%s: ppos=%08llx (fram=%d mode=%d addr=%x) count=%ld\n", __func__, *ppos, slot->fram_mode, mode, addr, count);


	/* SNES continuous OE changes neither mapper nor chip-select handling.
	 * Ensure the normal data bus is released before holding /OE low.
	 */
	if (hold_oe) {
		err = set_bus_bit(slot->bus->oe, 1);
		if (err)
			goto failed;
		end_drive_word(slot->bus);
	}

	/* The slot lock spans this read. No mapper writes or direction
	 * changes occur inside the fast NES byte loop. Release all data
	 * lines once, checking GPIO errors before starting any read cycle.
	 */
	if (fast_nes && count) {
		unsigned int d;

		for (d = 0; d < slot->bus->datas->ndescs; d++) {
			err = gpiod_direction_input(slot->bus->datas->desc[d]);
			if (err)
				goto failed;
		}
		if (mode == NES_PRG) {
			err = PRG_READ;
			if (err)
				goto failed;
		}
	}

	while (count > 0) {
		if (!((read + fill) & 0x3ff))
			retrode3_cart_led_activity(slot);

		if (mode == MD_ROM && slot->bus_width == 16 && count >= 2 && ((addr & 1) == 0)) {

			err = retrode3_set_rom_address(slot->bus, addr, bulk_address);
			if(err < 0)
				goto failed;

			err = read_word(slot->bus);
			if(err < 0)
				goto failed;

			buffer[fill++] = (u8)((u16)err >> 8);
			buffer[fill++] = (u8)err;
			addr += 2;
			count -= 2;
		} else {
			uint8_t byte;

			if (retrode3_adapter_mode_is_read(mode)) {
				/*
				 * An adapter can be unplugged during a block read.
				 * Stop issuing bus cycles as soon as slot sense drops.
				 */
				err = gpiod_get_value(slot->cd);
				if (err <= 0) {
					if (err == 0)
						err = -ENODEV;
					goto failed;
				}
				/*
				 * Explicit fixed 8-bit lane: no MD word reads and no
				 * A0-dependent byte-lane selection. read_half() pulses OE.
				 * No cartridge/mapper register write occurs here.
				 */
				err = _set_address(mode, slot, addr);
				if (err < 0)
					goto failed;
				byte = err = read_half(slot->bus,
					mode == RETRODE3_ADAPTER_READ_D0_D7 ? 1 : 0);
			} else if (slot->bus_width == 16) { // megadrive
				switch (mode) {
					case MD_TIME:
						err = _set_address(mode, slot, addr);	// 24 bit address and A0 determines lower/upper byte
						if(err < 0)
							goto failed;
						TIME_LOW;
						byte = err = read_byte(slot->bus);	// read half based on a0
						TIME_HIGH;
						break;
					// other modes
					case MD_ENSRAM:
						byte = err = slot->fram_mode;
						break;
					case MD_ROM:
					case MD_P1:
					case MD_P10:
						if (slot->fram_mode) {
//if ((addr &0xff) == 0) dev_info(&slot->dev, "%s: read FRAM MODE %08x %02x\n", __func__, addr, byte);
							/* NOTES:
							 * special SONIC3 FRAM mode
							 * IMPORTANT: if A10 = 0 and A21 = 1, writing the mapper in MD_TIME mode
							 *    may already enable the RAM-CE and lead to data loss!
							 * IMPORTANT: VCC should be above 3.5V - even for read
							 * falling edge of CE latches address
							 * OE activates bus driver
							 * OE is automatically handled by read_byte()
							 * rising edge refreshes data
							 * CE should not be activa for more than 10µs (tCA)
							 */
							TIME_HIGH;	// be sure we do not accidentally change the mapper
							MD_WE0_HIGH;	// make sure WE is High
							if (addr & 1) { // really read odd addresses (d0..d7)only!
								err = _set_address(mode, slot, addr);
								if(err < 0)
									goto failed;
								// CHECKME: the whole sequence where RAM-CE is active should not run more than 10 us!
								select_slot(slot->bus, slot);		// activate Cart Enable and RAM-CE to latch address
								byte = err = read_byte(slot->bus);	// read half based on a0 (pulsing OE)
								select_slot(slot->bus, NULL);		// deactivate RAM-CE of cart
							} else
								err = byte = 0x00;
//							dev_info(&slot->dev, "%s: read FRAM %08x %02x\n", __func__, addr, byte);
							break;
						}

						err = _set_address(mode, slot, addr);	// 24 bit address and A0 determines lower/upper byte
						if(err < 0)
							goto failed;
						switch (mode) {
							// hier MD_P10 und MD_P1 implementieren
							case MD_P1:
							case MD_P10:
								;
						}
						byte = err = read_byte(slot->bus);	// read half based on a0
			//			dev_info(&slot->dev, "%s: read BUS %08x %02x\n", __func__, addr, byte);
						break;
					default:
						dev_err(&slot->dev, "%s: mode (%d) not supported\n", __func__, mode);
						err = -EIO;
				}
			}
			else { // 8 bit bus SNES or NES
				switch (mode) {
					case MD_ROM:
						err = retrode3_set_rom_address(slot->bus, addr, bulk_address);
						if (err < 0)
							goto failed;
						if (!hold_oe) {
							byte = err = read_half(slot->bus, 1);
							break;
						}
						if (!oe_held) {
							err = set_bus_bit(slot->bus->oe, 0);
							if (err)
								goto failed;
							oe_held = true;
						}
						/* Settle after the final address transition. */
						udelay(1);
						byte = err = get_half(slot->bus, 1);
						if (err < 0)
							goto failed;
						break;
					case NES_PRG:
/*
static unsigned char read_prg_byte(unsigned int address) {
  MODE_READ;
  PRG_READ;
  ROMSEL_HI;
  set_address(address);
  PHI2_HI;
  set_romsel(address);
  _delay_us(1);
  return PINK;
}
*/

						if (fast_nes) {
							/* Preserve ROMSEL sequencing and the current
							 * M2-high PRG policy. Only the first byte
							 * needs to set M2; the bus stays in read mode.
							 */
							romsel = BIT(15);
							err = retrode3_nes_bulk_address(slot,
									slot->bus->current_addr);
							if (err)
								goto failed;
							err = retrode3_nes_bulk_address(slot, addr);
							if (err)
								goto failed;
							if (!read && !fill) {
								err = PHI2_HI;
								if (err)
									goto failed;
							}
							romsel = 0; /* CPU $8000-$FFFF */
							err = retrode3_nes_bulk_address(slot, addr);
							if (err)
								goto failed;
							udelay(1);
							/* Keep the existing OE pulse and settle
							 * in read_half() for this experiment.
							 */
							byte = err = PINK_PRG;
							break;
						}
						/* Experimental: mirror OSCR read_prg_byte(). Keep M2
						 * high across PRG reads; do not generate a long low
						 * pulse while configuring the GPIO data direction.
						 * Deassert ROMSEL before changing address, then select
						 * the ROM after M2 is high. No write-path changes.
						 */
						MODE_READ;
						PRG_READ;
						ROMSEL_HI;
						set_address(addr);
						PHI2_HI;
						set_romsel(addr);
						_delay_us(1);
						byte = err = PINK_PRG;	// NES CPU bus = D0..D7
						break;
					case NES_CHR:		// OSCR does not clock PHI2...
					case NES_CHR_M2:
/*
static unsigned char read_chr_byte(unsigned int address) {
  MODE_READ;
  PHI2_HI;
  ROMSEL_HI;
  set_address(address);
  CHR_READ_LOW;
  _delay_us(1);
  uint8_t result = PINK;
  CHR_READ_HI;
  return result;
}
*/
						if (fast_nes) {
							/* Preserve the legacy CHR M2 and OE
							 * transitions; only address, direction
							 * setup and settling implementation change.
							 */
							PHI2_LOW;
							PHI2_HI;
							romsel = BIT(15);
							err = retrode3_nes_bulk_address(slot,
									slot->bus->current_addr);
							if (err)
								goto failed;
							err = retrode3_nes_bulk_address(slot, addr);
							if (err)
								goto failed;
							CHR_READ_LOW;
							udelay(1);
							byte = err = PINK_CHR;
							CHR_READ_HI;
							break;
						}
						PHI2_LOW;		// CPU_M2 = L
						MODE_READ;
						PHI2_HI;
						ROMSEL_HI;
						set_address(addr);
						CHR_READ_LOW;
						_delay_us(1);
						byte = err = PINK_CHR;	// NES PPU bus = D8..D15
						CHR_READ_HI;
						break;
					case NES_MMC5_SRAM:
					case NES_REG:		// hier evtl. A15 = 1?
					case NES_RAM:
					case NES_WRAM:
					default:
						dev_err(&slot->dev, "%s: mode (%d) not supported\n", __func__, mode);
						err = -EIO;
				}
			}
			if(err < 0)
				goto failed;

			buffer[fill++] = byte;
			addr++;
			count--;
		}

		/* Leave room for a word on the next iteration. Advance the file
		 * offset only for bytes successfully copied to userspace.
		 */
		if (fill >= sizeof(buffer) - 1 || !count) {
			size_t remaining, copied;

			/* Never leave /OE active across a possibly faulting user copy. */
			if (oe_held) {
				err = set_bus_bit(slot->bus->oe, 1);
				if (err)
					goto failed;
				oe_held = false;
			}
			remaining = copy_to_user(buf + read, buffer, fill);
			copied = fill - remaining;

			read += copied;
			*ppos += copied;
			fill = 0;
			if (remaining) {
				err = -EFAULT;
				goto failed;
			}
		}
	}

	if (bridge_md_ce) {
		int restore;

		/* Stop RD before MD CE is driven inactive again. */
		set_bus_bit(slot->bus->oe, 1);
		restore = gpiod_direction_output(bridge_md_ce, 0);
		if (restore < 0) {
			dev_err(&slot->dev,
				"bridge test: MD CE restore failed: %d\n", restore);
			select_slot(slot->bus, NULL);
			return restore;
		}
	}
	select_slot(slot->bus, NULL);

	return read;

failed:
	if (hold_oe)
		set_bus_bit(slot->bus->oe, 1);
	if (bridge_md_ce) {
		int restore;

		set_bus_bit(slot->bus->oe, 1);
		restore = gpiod_direction_output(bridge_md_ce, 0);
		if (restore < 0)
			dev_err(&slot->dev,
				"bridge test: MD CE restore failed: %d\n", restore);
	}
	select_slot(slot->bus, NULL);

	return read ? read : err;
}

/*
 * Experimental SMS-0007 mapper-register write.
 * Invoked ONLY by mode 22 (not the generic MD write path).
 * A23 is SMS /RD (held HIGH), A22 is SMS /WR (pulsed LOW);
 * common OE drives the adapter's /CE while MD's own CE is Hi-Z.
 *
 * USB/Web autoprobe, 3.3V, other addresses, and ALL other write
 * modes remain blocked. Other populated slots are deselected below;
 * their presence does not independently prohibit this MD-slot operation.
 * Requires oscilloscope validation before any live experimental use.
 */
static ssize_t retrode3_sms_mapper_write(struct retrode3_slot *slot,
					 const char __user *buf,
					 size_t count, loff_t *ppos,
					 u32 addr)
{
	struct retrode3_bus *bus = slot->bus;
	const u32 base = BIT(23) | BIT(22) | BIT(17) | (0xfffcu << 1);
	struct retrode3_slot *other;
	u8 value;
	int err = 0, ret, i;
	u32 reg;
	const char *failed_stage = "address_setup";

	if (!legacy_bridge_hiz_test || !sms_mapper_write_test)
		return -EOPNOTSUPP;
	if (!of_device_is_compatible(slot->dev.of_node,
			"openpandora,retrode3-slot-megadrive") ||
	    slot->bus_width != 16 || slot->fram_mode || count != 1 ||
	    (addr & ~6u) != base || (addr & 1))
		return -EOPNOTSUPP;
	reg = (addr & 6u) >> 1;
	if (copy_from_user(&value, buf, 1))
		return -EFAULT;

	/* Sanni's initialization: SRAM off, pages 0/1 fixed, page 2 banked. */
	if ((reg == 0 && value != 0) ||
	    (reg == 1 && value != 0) ||
	    (reg == 2 && value != 1) ||
	    (reg == 3 && value >= 64))
		return -EPERM;

	if (get_slot_power_mV(slot) != 5000)
		return -EPERM;
	ret = gpiod_get_value(slot->cd);
	if (ret <= 0) {
		if (ret < 0)
			dev_err(&slot->dev, "SMS mapper: cart detect GPIO error %d\n", ret);
		return ret < 0 ? ret : -ENODEV;
	}

	mutex_lock(&bus->select_lock);
	/* Control lines idle, data inputs and MD CE inactive before setup. */
	set_bus_bit(bus->oe, 1);
	gpiod_set_value(bus->we->desc[0], 0);
	gpiod_set_value(bus->we->desc[1], 0);
	gpiod_set_value(bus->time, 0);
	end_drive_word(bus);

	for (i = 0; i < ARRAY_SIZE(bus->slots); i++) {
		other = bus->slots[i];
		if (other && !other->is_controller && !IS_ERR_OR_NULL(other->ce))
			gpiod_set_value(other->ce, 0);
	}

	err = _set_address(RETRODE3_ADAPTER_SMS_MD_MAPPER_WRITE, slot, addr);
	if (err < 0)
		goto restore;

	failed_stage = "md_ce_high_z";
	err = gpiod_direction_input(slot->ce);
	if (err < 0)
		goto restore;

	/* A17 high prevents RAM selection; A23 high keeps /RD inactive. */
	failed_stage = "data_drive";
	for (i = 0; i < 8; i++) {
		err = gpiod_direction_output(bus->datas->desc[i],
						 (value >> i) & 1);
		if (err < 0) {
			dev_err(&slot->dev,
				"SMS mapper: data GPIO %d direction error %d\n", i, err);
			goto restore;
		}
	}
	udelay(1);
	/* Apply /WR only while /CE is inactive. */
	failed_stage = "wr_address_setup";
	err = _set_address(RETRODE3_ADAPTER_SMS_MD_MAPPER_WRITE,
			   slot, addr & ~BIT(22));
	if (err < 0)
		goto restore;
	udelay(1);
	set_bus_bit(bus->oe, 0);	/* Adapter /CE asserted */
	udelay(1);
	set_bus_bit(bus->oe, 1);	/* Adapter /CE released */

restore:
	/* Never restore data/MD CE until RD is inactive and /WR is high. */
	set_bus_bit(bus->oe, 1);
	ret = _set_address(RETRODE3_ADAPTER_SMS_MD_MAPPER_WRITE, slot, addr);
	if (ret < 0) {
		dev_err(&slot->dev, "SMS mapper: address restore error %d\n", ret);
		if (err >= 0) {
			err = ret;
			failed_stage = "address_restore";
		}
	}
	end_drive_word(bus);
	ret = gpiod_direction_output(slot->ce, 0);
	if (ret < 0) {
		dev_err(&slot->dev, "SMS mapper: MD CE restore error %d\n", ret);
		if (err >= 0) {
			err = ret;
			failed_stage = "md_ce_restore";
		}
	}
	mutex_unlock(&bus->select_lock);
	if (err < 0) {
		dev_err(&slot->dev, "SMS mapper: step=%s reg=%u error=%d\n",
			failed_stage, reg, err);
		return err;
	}
	*ppos += 1;
	return 1;
}

/*
 * Native X1600 NES mapper write timing. ROMSEL (PA31) and M2 (PC26)
 * are on different GPIO banks: simultaneous edges are impossible here.
 * Qualify the exact descriptor layout and use consecutive MMIO SET/CLEAR
 * accesses with IRQs disabled only for the short active write pulse.
 * No scheduler sleep between M2 falling and ROMSEL rising.
 *
 * Only single-byte NES_PRG writes at the register addresses needed by
 * MMC3/SMD133 use this path. No new writable address or write mode is
 * introduced. Keep other addresses, SRAM modes and other hardware legacy.
 */
static bool retrode3_nes_tight_write_allowed(struct retrode3_slot *slot,
					     u32 mode, u32 addr, size_t count)
{
	struct retrode3_bus *bus = slot->bus;
	struct gpio_chip *gc, *ga;

	if ((mode != NES_PRG && mode != NES_REG) || count != 1 ||
	    slot->bus_width != 8 ||
	    !of_device_is_compatible(slot->dev.of_node,
				     "openpandora,retrode3-slot-nes") ||
	    !bus->rom_addr_bulk_available || !bus->rom_read_bank_map ||
	    IS_ERR_OR_NULL(bus->phi2) || gpiod_cansleep(bus->phi2) ||
	    IS_ENABLED(CONFIG_PREEMPT_RT))
		return false;
	/* MMC5 ROM banking: use the same bounded M2-low write primitive
	 * as SMD133. Do not extend this to RAM-protect, SRAM or ExRAM
	 * registers; userspace still owns board-specific write policies.
	 * Actual pulse widths on MMC5 hardware remain to be measured.
	 */
	if (mode == NES_REG && addr != 0xe000u && addr != 0xf800u)
		return false;
	if (mode == NES_PRG && addr != 0x8000u && addr != 0x8001u && addr != 0xa001u &&
	    !(addr >= 0x5000u && addr <= 0x5003u) &&
	    !(addr >= 0x6000u && addr <= 0x6003u) &&
	    addr != 0x5100u && addr != 0x5101u &&
	    addr != 0x5114u && addr != 0x5115u &&
	    addr != 0x5127u && addr != 0x5130u)
		return false;

	/* The bulk-address probe already verifies PA31 and the X1600 map. */
	ga = bus->addrs->desc[15]->gdev->chip;
	gc = bus->phi2->gdev->chip;
	return gc && ga && gc->parent && gc->parent == ga->parent &&
	       gc->label && !strcmp(gc->label, "GPIOC") &&
	       gc->ngpio == 32 && gpiod_hwgpio(bus->phi2) == 26 &&
	       gpiod_is_active_low(bus->phi2);
}

static int retrode3_nes_tight_write(struct retrode3_slot *slot,
				    u32 addr, u8 byte)
{
	struct retrode3_bus *bus = slot->bus;
	struct regmap *map = bus->rom_read_bank_map;
	const unsigned int pc = 2 * R3_ROM_GPIO_BANK_STRIDE;
	unsigned long flags;
	bool select_rom = addr & BIT(15);
	int err, ret;
	unsigned int d;

	/* Physical M2 low, ROMSEL high, then address/data setup.
	 * The logical GPIO API accounts for the active-low M2 and R/W.
	 */
	err = gpiod_set_value(bus->phi2, 1);
	if (err)
		goto restore;
	romsel = BIT(15);
	err = _set_address(NES_PRG, slot, addr);
	if (err)
		goto restore;
	err = gpiod_set_value(bus->we->desc[0], 1);
	if (err)
		goto restore;
	for (d = 0; d < 8; d++) {
		err = gpiod_direction_output(bus->datas->desc[d],
					     (byte >> d) & 1);
		if (err)
			goto restore;
	}
	udelay(1);

	/* Match the physical NES order: M2 rises then /ROMSEL asserts;
	 * M2 falls then /ROMSEL releases. Below $8000 ROMSEL stays high.
	 * MMIO regmap is non-sleeping on the qualified X1600 controller.
	 * Address and data have settled before this IRQ-off section.
	 */
	local_irq_save(flags);
	err = regmap_write(map, pc + R3_ROM_PAT0_SET, BIT(26));
	if (!err && select_rom)
		err = regmap_write(map, R3_ROM_PAT0_CLEAR, BIT(31));
	if (!err)
		udelay(1);
	ret = regmap_write(map, pc + R3_ROM_PAT0_CLEAR, BIT(26));
	if (!err)
		err = ret;
	if (select_rom) {
		ret = regmap_write(map, R3_ROM_PAT0_SET, BIT(31));
		if (!err)
			err = ret;
	}
	local_irq_restore(flags);
	/* ROMSEL is back at its cached high level. An incomplete MMIO
	 * transaction invalidates the address cache before any retry.
	 */
	if (err)
		bus->rom_addr_cache_dirty = true;
	udelay(1);

restore:
	/* Restore the same idle CPU read cycle as the legacy primitive.
	 * On errors, keep M2 low, release the data bus and report failure.
	 */
	ret = gpiod_set_value(bus->phi2, 1);
	if (!err)
		err = ret;
	romsel = BIT(15);
	ret = _set_address(NES_PRG, slot, addr);
	if (!err)
		err = ret;
	ret = gpiod_set_value(bus->we->desc[0], 0);
	if (!err)
		err = ret;
	for (d = 0; d < bus->datas->ndescs; d++) {
		ret = gpiod_direction_input(bus->datas->desc[d]);
		if (!err)
			err = ret;
	}
	if (err)
		return err;
	err = _set_address(NES_PRG, slot, 0);
	if (err)
		return err;
	return gpiod_set_value(bus->phi2, 0);
}

/*
 * Reset-sensitive mapper experiment: never spend GPIO direction setup
 * or release time with M2 low. Keep the prior tight-write primitive as
 * a runtime A/B fallback. Only its already approved addresses qualify.
 */
static bool r3_nes_short_m2 = true;
module_param_named(nes_short_m2, r3_nes_short_m2, bool, 0600);
MODULE_PARM_DESC(nes_short_m2,
	"NES mapper writes: short M2 low phases on qualified X1600 (default on)");

static bool retrode3_nes_short_m2_allowed(struct retrode3_slot *slot)
{
	struct retrode3_bus *bus = slot->bus;
	struct gpio_desc *rw = bus->we->desc[0];
	struct gpio_chip *gc, *ga;

	if (!READ_ONCE(r3_nes_short_m2) || IS_ERR_OR_NULL(rw) ||
	    gpiod_cansleep(rw) || !gpiod_is_active_low(rw) ||
	    bus->datas->ndescs != 16)
		return false;
	/* Previous tight-write guard has qualified M2 and ROMSEL.
	 * Rev 2.9.5 CPU R/W is physical PB8, active-low in the DT.
	 */
	gc = rw->gdev->chip;
	ga = bus->addrs->desc[15]->gdev->chip;
	return gc && ga && gc->parent && gc->parent == ga->parent &&
	       gc->label && !strcmp(gc->label, "GPIOB") &&
	       gc->ngpio == 32 && gpiod_hwgpio(rw) == 8;
}

static int retrode3_nes_short_m2_write(struct retrode3_slot *slot,
				       u32 addr, u8 byte, bool reg_strobe)
{
	struct retrode3_bus *bus = slot->bus;
	struct regmap *map = bus->rom_read_bank_map;
	const unsigned int pb = R3_ROM_GPIO_BANK_STRIDE;
	const unsigned int pc = 2 * R3_ROM_GPIO_BANK_STRIDE;
	const bool select_rom = addr & BIT(15);
	unsigned long flags;
	unsigned int d;
	int err, ret;

	/* Establish an inactive cartridge read at CPU $0000, outside
	 * cartridge CPU address space, before driving the CPU data lane.
	 * No PPU output or write strobe is enabled.
	 */
	err = gpiod_set_value(bus->we->desc[0], 0);
	if (err)
		goto failed;
	err = set_bus_bit(bus->oe, 1);
	if (err)
		goto failed;
	err = gpiod_set_value(bus->we->desc[1], 0);
	if (err)
		goto failed;
	romsel = BIT(15);
	err = retrode3_nes_bulk_address(slot, 0);
	if (err)
		goto failed;
	err = gpiod_set_value(bus->phi2, 0);
	if (err)
		goto failed;
	for (d = 0; d < 8; d++) {
		err = gpiod_direction_output(bus->datas->desc[d],
					     (byte >> d) & 1);
		if (err)
			goto failed;
	}
	udelay(1);

	/* Address, data, and R/W transitions use non-sleeping MMIO.
	 * All GPIO direction operations are outside the IRQ-off window.
	 * The two low intervals contain only the required bus transitions.
	 */
	local_irq_save(flags);
	err = regmap_write(map, pc + R3_ROM_PAT0_CLEAR, BIT(26));
	if (!err)
		err = retrode3_nes_bulk_address(slot, addr);
	if (!err)
		err = regmap_write(map, pb + R3_ROM_PAT0_CLEAR, BIT(8));
	if (!err) {
		ndelay(100);
		err = regmap_write(map, pc + R3_ROM_PAT0_SET, BIT(26));
	}
	if (!err && select_rom)
		err = regmap_write(map, R3_ROM_PAT0_CLEAR, BIT(31));
	/* Sanni write_reg_byte uses consecutive port writes, not the
	 * ordinary 1 us high hold. Separate X1600 GPIO banks cannot
	 * guarantee AVR's simultaneous edges or 33 ns skew. */
	if (!err && !reg_strobe)
		udelay(1);

	/* End the write, deassert ROMSEL, return R/W to read and move
	 * away from cartridge address space BEFORE raising idle M2.
	 */
	ret = regmap_write(map, pc + R3_ROM_PAT0_CLEAR, BIT(26));
	if (!err)
		err = ret;
	if (select_rom) {
		ret = regmap_write(map, R3_ROM_PAT0_SET, BIT(31));
		if (!err)
			err = ret;
	}
	if (reg_strobe && !err)
		udelay(1); /* original write_reg_byte post-strobe hold */
	ret = regmap_write(map, pb + R3_ROM_PAT0_SET, BIT(8));
	if (!err)
		err = ret;
	if (err)
		bus->rom_addr_cache_dirty = true;
	if (!err)
		err = retrode3_nes_bulk_address(slot, 0);
	if (!err)
		err = regmap_write(map, pc + R3_ROM_PAT0_SET, BIT(26));
	local_irq_restore(flags);
	if (err)
		goto failed;

	/* CPU $0000 is still deselected; release the data lane with M2
	 * already high so pinctrl latency cannot prolong M2 low.
	 */
	for (d = 0; d < bus->datas->ndescs; d++) {
		ret = gpiod_direction_input(bus->datas->desc[d]);
		if (!err)
			err = ret;
	}
	if (!err)
		return 0;

failed:
	/* Fail closed: M2 low, CPU read, ROM deselected, data inputs.
	 * Re-establish physical address levels if a partial MMIO failed.
	 */
	bus->rom_addr_cache_dirty = true;
	gpiod_set_value(bus->phi2, 1);
	gpiod_set_value(bus->we->desc[0], 0);
	romsel = BIT(15);
	_set_address(NES_PRG, slot, 0);
	end_drive_word(bus);
	return err;
}

static ssize_t retrode3_write(struct file *file, const char __user *buf,
			 size_t count, loff_t *ppos)
{
	struct retrode3_file_context *ctx = file->private_data;
	struct retrode3_slot *slot = ctx->slot;
	uint32_t addr = *ppos & 0xffffff;	/* max address is 24 bit */
	uint32_t mode = *ppos >> 24;		/* access mode control */
	ssize_t written, sz;
	unsigned long copied;
	int err;
	bool tight_nes = retrode3_nes_tight_write_allowed(slot, mode, addr, count);

// dev_info(&slot->dev, "%s: ppos=%08llx (fram=%d mode=%d addr=%x) count=%ld\n", __func__, *ppos, slot->fram_mode, mode, addr, count);

	written = 0;

	/* No writes of any kind during a GBx bus-direction experiment. */
	if (gbx_md_test || slot->bus->gbx_active)
		return -EBUSY;

	/* Independent narrowly guarded mapper mode; no other writes enabled. */
	if (mode == RETRODE3_ADAPTER_SMS_MD_MAPPER_WRITE)
		return retrode3_sms_mapper_write(slot, buf, count, ppos, addr);

	/* No cart writes are permitted in the bench experiment. */
	if (legacy_bridge_hiz_test)
		return -EOPNOTSUPP;

	/* The legacy NES_REG stub never generated a real strobe. Limit
	 * this write-only mode to the two reviewed ROM bank registers.
	 * Do not silently fall back to the slow GPIO path. This is NOT
	 * an electrically qualified save-RAM-safe mode. */
	if (mode == NES_REG) {
		u8 data;

		if (!tight_nes || slot->fram_mode || !slot->bus->we ||
		    slot->bus->we->ndescs != 2 ||
		    !retrode3_nes_short_m2_allowed(slot) ||
		    !retrode3_nes_pulse_allowed(slot))
			return -EOPNOTSUPP;
		if (copy_from_user(&data, buf, 1))
			return -EFAULT;
		if ((addr == 0xf800u && data >= 8u) ||
		    (addr == 0xe000u && data >= 16u))
			return -EINVAL;
		select_slot(slot->bus, slot);
		err = gpiod_get_value(slot->cd);
		if (err > 0) {
			retrode3_cart_led_activity(slot);
			err = retrode3_nes_short_m2_write(slot, addr, data, true);
		} else if (!err) {
			err = -ENODEV;
		}
		select_slot(slot->bus, NULL);
		if (err)
			return err;
		*ppos += 1;
		return 1;
	}

	/* Dedicated pulse operations are bounded before selecting hardware. */
	if (mode == NES_PRG_PULSE || mode == NES_ADDR_STROBE)
		return retrode3_nes_pulse_write(slot, buf, count, ppos, mode, addr);
	if (mode == NES_CHR_PULSE)
		return -EOPNOTSUPP;
	/* Adapter lane modes must never perform a write cycle. */
	if (retrode3_adapter_mode_is_read(mode))
		return -EOPNOTSUPP;

	if ((slot->bus_width != 16 || !slot->fram_mode) && mode != MD_ENSRAM)
		select_slot(slot->bus, slot);	// activate cart enable

	if (addr + count >= EOF) {
		if (addr >= EOF)
			count = 0;	// write nothing
		else
			count = EOF - addr;	// limit to EOF
	}

if ((addr &0xff) == 0) dev_info(&slot->dev, "%s: write mode=%02x %08x %08x\n", __func__, mode, addr, count);
	while (count > 0) {
		if (!(written & 0x3ff))
			retrode3_cart_led_activity(slot);
		// int allowed;
		uint8_t byte;

// FIXME: loop over bytes and write them to consecutive addresses
// handle words for 16 bit bus and faster write

		sz = sizeof(byte);

		copied = copy_from_user(&byte, buf, sz);

		switch (mode) {
			case MD_ROM:
				if (slot->fram_mode) {
//					dev_info(&slot->dev, "%s: write FRAM %08x %02x\n", __func__, addr, byte);
//if ((addr &0xff) == 0) dev_info(&slot->dev, "%s: write FRAM MODE %08x %02x\n", __func__, addr, byte);
					/* NOTES:
					 * special SONIC3 FRAM mode
					 * IMPORTANT: if A10 = 0 and A21 = 1, writing the mapper in MD_TIME mode
					 *    may already enable the RAM-CE and lead to data loss!
					 * IMPORTANT: VCC should be above 3.5V - even for read
					 * we use "CE Controlled Write"
					 * WE must be active before CE, OE is ignored
					 * falling edge of CE latches address
					 * rising edge writes data
					 * CE should not be activa for more than 10µs (tCA)
					 */
					TIME_HIGH;	// be sure we do not accidentally change the mapper
					MD_WE0_HIGH;	// make sure WE is High
					if (addr & 1) { // really write odd addresses only!

						err = _set_address(mode, slot, addr);	// this could lead to falling edge of RAM-CE (if cart is selected and mapper enabled)
						if(err < 0)
							goto failed;
						MD_WE0_LOW;	// activate WE ("CE Controlled Write" mode)
						// CHECKME: the whole sequence where RAM-CE is active should not run more than 10 us!
						select_slot(slot->bus, slot);	// activate Cart Enable and RAM-CE to latch address
						set_half(slot->bus, byte, slot->bus->current_addr & 1);	// a0 should be 1 for D0..D7 but no WE0
						select_slot(slot->bus, NULL);	// deactivate RAM-CE of cart
						MD_WE0_HIGH;	// make sure WE is High
						end_drive_word(slot->bus);	// deactivate data lines as output
					}
					break;
				}
				dev_info(&slot->dev, "%s: write BUS %08x %02x\n", __func__, addr, byte);
				err = _set_address(mode, slot, addr);	// 24 bit address and A0 determines lower/upper byte
				if(err < 0)
					goto failed;
				write_byte(slot->bus, byte);	// a0 should be 1 for D0..D7 and WE0
// CHECKME: do we have to play the WE0/WE8 differently?
				break;
			case MD_TIME:	// write with active TIME impulse
				dev_info(&slot->dev, "%s: write MD_TIME %08x %02x\n", __func__, addr, byte);
				err = _set_address(mode, slot, addr);	// 24 bit address and A0 determines lower/upper byte
				if(err < 0)
					goto failed;
				TIME_LOW;
				write_byte(slot->bus, byte);	// a0 should be 1 for D0..D7 and WE0
				TIME_HIGH;
				break;
				;;
			case MD_ENSRAM:	// write to D0..D7 with active TIME impulse but no WE0
//				dev_info(&slot->dev, "%s: write MD_ENSRAM %08x %02x\n", __func__, addr, byte);
				// IMPORTANT: if A10 = 0 and A21 = 1, writing the mapper in MD_TIME mode may already enable the CE and lead to data loss!
				// so make sure that if you write in MD_TIME mode with either A10 = 1 or A21 = 0 (0x03a13001 is NOT AT ALL safe)
				select_slot(slot->bus, NULL);	// deactivate cart enable (if still enabled)
#if 0	// ignore address
				err = _set_address(mode, slot, addr);	// 24 bit address and A0 determines lower/upper byte
				if(err < 0)
					goto failed;
#endif
				err = _set_address(mode, slot, 1<<10);	// make sure we do NOT have A10 = 0 or A21 = 1 as this might activate CE too early
				if(err < 0)
					goto failed;
				set_half(slot->bus, byte, 1);	// set to D0..D7
				TIME_LOW;
				_delay_us(1);	// there is no WE0!
				TIME_HIGH;
				end_drive_word(slot->bus);
				slot->fram_mode = byte != 0;	// remember
// printk("%s: fram_mode=%d\n", __func__, slot->fram_mode);
				break;
				;;
			case NES_PRG:
			case NES_MMC5_SRAM:
				if (tight_nes && copied) {
					err = -EFAULT;
					goto failed;
				}
				dev_info(&slot->dev, "%s: write NES CPU/PRG/SRAM %08x %02x\n", __func__, addr, byte);
				if (tight_nes) {
					if (retrode3_nes_short_m2_allowed(slot))
						err = retrode3_nes_short_m2_write(slot, addr, byte, false);
					else
						err = retrode3_nes_tight_write(slot, addr, byte);
					if (err)
						goto failed;
					break;
				}
				// err = write_prg_byte(addr, byte);

/*
static void write_prg_byte(unsigned int address, uint8_t data) {
  PHI2_LOW;
  ROMSEL_HI;
  MODE_WRITE;
  PRG_WRITE;
  PORTK = data;

  set_address(address);  // PHI2 low, ROMSEL always HIGH
  //  _delay_us(1);
  PHI2_HI;
  //_delay_us(10);
  set_romsel(address);  // ROMSEL is low if need, PHI2 high
  _delay_us(1);         // WRITING
  //_delay_ms(1); // WRITING
  // PHI2 low, ROMSEL high
  PHI2_LOW;
  _delay_us(1);
  ROMSEL_HI;
  // Back to read mode
  //  _delay_us(1);
  PRG_READ;
  MODE_READ;
  set_address(0);
  // Set phi2 to high state to keep cartridge unreseted
  //  _delay_us(1);
  PHI2_HI;
  //  _delay_us(1);
}
*/

				PHI2_LOW;		// CPU_M2 = L
				ROMSEL_HI;		// disable ROM
				MODE_WRITE;		// switch data to output
				PRG_WRITE;		// activate CPU_R/W (WE0)
				PORTK_PRG(byte);	// set data bus
				set_address(addr);	// set address
				//  _delay_us(1);
				PHI2_HI;		// this edge should clock data into mapper
				//_delay_us(10);
				set_romsel(addr);	// ROMSEL is low if need, PHI2 high
				_delay_us(1);		// WRITING
				//_delay_ms(1); // WRITING
				// PHI2 low, ROMSEL high
				PHI2_LOW;		// start new (read) cycle
				_delay_us(1);
				ROMSEL_HI;		// disable ROM
				// Back to read mode
				//  _delay_us(1);
				PRG_READ;
				MODE_READ;
				set_address(0);
				// Set phi2 to high state to keep cartridge unreseted
				//  _delay_us(1);
				PHI2_HI;		// this clocks a dummy read to prepare the mapper
				//  _delay_us(1);
				break;
			case NES_CHR:
			case NES_CHR_M2:	// FIXME: what is the difference? Clocking M2 = CE-NES
/*
static void write_chr_byte(unsigned int address, uint8_t data) {
  PHI2_LOW;
  ROMSEL_HI;
  MODE_WRITE;
  PORTK = data;
  set_address(address);  // PHI2 low, ROMSEL always HIGH
  _delay_us(1);

  CHR_WRITE_LOW;

  _delay_us(1);  // WRITING

  CHR_WRITE_HI;

  _delay_us(1);

  MODE_READ;
  set_address(0);
  PHI2_HI;

  //_delay_us(1);
}

*/
				dev_info(&slot->dev, "%s: write NES PPU/CHR %08x %02x\n", __func__, addr, byte);
				PHI2_LOW;
				ROMSEL_HI;
				MODE_WRITE;
				PORTK_CHR(byte);
 				set_address(addr);
				_delay_us(1);

				CHR_WRITE_LOW;

				_delay_us(1);  // WRITING

				CHR_WRITE_HI;

				_delay_us(1);

				MODE_READ;
				set_address(0);
				PHI2_HI;

				//_delay_us(1);
				break;
			case NES_RAM:
/*
static void write_ram_byte(unsigned int address, uint8_t data) {  // Mapper 19 (Namco 106/163) WRITE RAM SAFE ($E000-$FFFF)
  PHI2_LOW;
  ROMSEL_HI;
  MODE_WRITE;
  PRG_WRITE;
  PORTK = data;

  set_address(address);  // PHI2 low, ROMSEL always HIGH
  PHI2_HI;
  ROMSEL_LOW;    // SET /ROMSEL LOW OTHERWISE CORRUPTS RAM
  _delay_us(1);  // WRITING
  // PHI2 low, ROMSEL high
  PHI2_LOW;
  _delay_us(1);
  ROMSEL_HI;
  // Back to read mode
  PRG_READ;
  MODE_READ;
  set_address(0);
  // Set phi2 to high state to keep cartridge unreseted
  PHI2_HI;
}
*/
				PHI2_LOW;
				ROMSEL_HI;
				MODE_WRITE;
				PRG_WRITE;
				PORTK_PRG(byte);
				set_address(addr);  // PHI2 low, ROMSEL always HIGH
				PHI2_HI;
				ROMSEL_LOW;    // SET /ROMSEL LOW OTHERWISE CORRUPTS RAM
				_delay_us(1);  // WRITING
				// PHI2 low, ROMSEL high
				PHI2_LOW;
				_delay_us(1);
				ROMSEL_HI;
				// Back to read mode
				PRG_READ;
				MODE_READ;
				set_address(0);
				// Set phi2 to high state to keep cartridge unreseted
				PHI2_HI;
				break;
			case NES_WRAM:
/*
static void write_wram_byte(unsigned int address, uint8_t data) {  // Mapper 5 (MMC5) RAM
  PHI2_LOW;
  ROMSEL_HI;
  set_address(address);
  PORTK = data;

  _delay_us(1);
  MODE_WRITE;
  PRG_WRITE;
  PHI2_HI;
  _delay_us(1);  // WRITING
  PHI2_LOW;
  ROMSEL_HI;
  // Back to read mode
  PRG_READ;
  MODE_READ;
  set_address(0);
  // Set phi2 to high state to keep cartridge unreseted
  PHI2_HI;
}
*/

				PHI2_LOW;
				ROMSEL_HI;
				set_address(addr);
				PORTK_PRG(byte);
				_delay_us(1);
				MODE_WRITE;
				PRG_WRITE;
				PHI2_HI;
				_delay_us(1);  // WRITING
				PHI2_LOW;
				ROMSEL_HI;
				// Back to read mode
				PRG_READ;
				MODE_READ;
 				set_address(0);
				// Set phi2 to high state to keep cartridge unreseted
				PHI2_HI;
				break;

/*

// NOTE: there are even more modes in NES.ino
// write_reg_m59()
// write_prg_pulsem2()
// read_prg_pulsem2()
// read_chr_pulsem2()

*/

			default:
				dev_err(&slot->dev, "%s: mode (%d) not implemented\n", __func__, mode);
				err = -EIO;
				goto failed;
		}

		if (copied) {
			written += sz - copied;
			if (written)
				break;
			err = -EFAULT;
			goto failed;
		}

		*ppos += sz;
		addr += sz;
		buf += sz;
		count -= sz;
		written += sz;
	}

	select_slot(slot->bus, NULL);

	return written;
failed:
	select_slot(slot->bus, NULL);

	return err;
}

/*
 * The memory devices use the full 32/64 bits of the offset, and so we cannot
 * check against negative addresses: they are ok. The return value is weird,
 * though, in that case (0).
 *
 * also note that seeking relative to the "end of file" isn't supported:
 * it has no meaning, so it returns -EINVAL.
 */
static loff_t retrode3_lseek(struct file *file, loff_t offset, int orig)
{
	loff_t ret;

	inode_lock(file_inode(file));
	switch (orig) {
	case SEEK_CUR:
		offset += file->f_pos;
		fallthrough;
	case SEEK_SET:
		/* to avoid userland mistaking f_pos=-9 as -EBADF=-9 */
		if ((unsigned long long)offset >= -MAX_ERRNO) {
			ret = -EOVERFLOW;
			break;
		}
		file->f_pos = offset;
		ret = file->f_pos;
		force_successful_syscall_return();
		break;
	default:
		ret = -EINVAL;
	}
	inode_unlock(file_inode(file));
	return ret;
}

static int retrode3_open(struct inode *inode, struct file *file)
{
	struct retrode3_file_context *ctx;
	struct retrode3_slot *slot;
	int ret = 0;

	slot = container_of(inode->i_cdev, struct retrode3_slot, cdev);

        dev_dbg(&slot->dev, "%s\n", __func__);

	ret = gpiod_get_value(slot->cd);	// check cart detect
	if (ret < 0)
		return ret;

	/*
	 * GBx-0011 uses the MD socket as a generic GB/GBC/GBA transport.
	 * The physical MD cartridge detect switch may stay open for a
	 * GBx adapter, especially with GBA cartridges. Allow open only
	 * for an explicitly armed, dedicated, read-only GBx bench on MD.
	 * Normal read/write modes remain blocked by gbx_md_test, and
	 * gbx_read validates the physical MD slot and voltage again.
	 *
	 * In this mode absence must be determined by the validated GB/GBA
	 * ROM header, not by the MD cartridge-detect GPIO.
	 */
	if (!ret && !(gbx_md_test && retrode3_gbx_md_slot(slot)))
		return -ENODEV;

	get_device(&slot->dev);	// increment the reference count for the device

	ret = generic_file_open(inode, file);
	if (ret < 0) {
		put_device(&slot->dev);
		return ret;
	}

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		put_device(&slot->dev);
		return -ENOMEM;
	}
	ctx->slot = slot;
	ctx->gbx_profile = gbx_md_test && retrode3_gbx_md_slot(slot);
	if (ctx->gbx_profile)
		gbx_profile_users++;
	file->private_data = ctx;

	return ret;
}

static int retrode3_release(struct inode *inode, struct file *file)
{
	struct retrode3_file_context *ctx = file->private_data;
	struct retrode3_slot *slot = ctx->slot;

        dev_dbg(&slot->dev, "%s\n", __func__);

	if (ctx->gbx_profile)
		gbx_profile_users--;
	put_device(&slot->dev);
	kfree(ctx);

	return 0;
}

/* Optional x16 MD CFI bench ioctl. See retrode3_flash_ioctl.h. */
#include "retrode3_flash.c"

/* Profile transitions and all cartridge file operations share this lock.
 * The existing slot mutex still owns physical chip selection.
 */
static ssize_t retrode3_scoped_read(struct file *file, char __user *buf,
				    size_t count, loff_t *ppos)
{
	ssize_t ret;

	mutex_lock(&gbx_profile_lock);
	/* A legacy raw sysfs session may own select_lock across calls.
	 * Do not wait for it while holding the profile mutex.
	 */
	if (mutex_is_locked(&((struct retrode3_file_context *)file->private_data)->slot->bus->select_lock))
		ret = -EBUSY;
	else
		ret = retrode3_read(file, buf, count, ppos);
	mutex_unlock(&gbx_profile_lock);
	return ret;
}

static ssize_t retrode3_scoped_write(struct file *file, const char __user *buf,
				     size_t count, loff_t *ppos)
{
	ssize_t ret;

	mutex_lock(&gbx_profile_lock);
	/* A legacy raw sysfs session may own select_lock across calls.
	 * Do not wait for it while holding the profile mutex.
	 */
	if (mutex_is_locked(&((struct retrode3_file_context *)file->private_data)->slot->bus->select_lock))
		ret = -EBUSY;
	else
		ret = retrode3_write(file, buf, count, ppos);
	mutex_unlock(&gbx_profile_lock);
	return ret;
}

static long retrode3_scoped_ioctl(struct file *file, unsigned int cmd,
				  unsigned long arg)
{
	long ret;

	mutex_lock(&gbx_profile_lock);
	ret = (gbx_md_test || mutex_is_locked(&((struct retrode3_file_context *)file->private_data)->slot->bus->select_lock)) ?
		-EBUSY : retrode3_flash_ioctl(file, cmd, arg);
	mutex_unlock(&gbx_profile_lock);
	return ret;
}

static int retrode3_scoped_open(struct inode *inode, struct file *file)
{
	int ret;

	mutex_lock(&gbx_profile_lock);
	ret = retrode3_open(inode, file);
	mutex_unlock(&gbx_profile_lock);
	return ret;
}

static int retrode3_scoped_release(struct inode *inode, struct file *file)
{
	int ret;

	mutex_lock(&gbx_profile_lock);
	ret = retrode3_release(inode, file);
	mutex_unlock(&gbx_profile_lock);
	return ret;
}

static const struct file_operations slot_fops = {
	.llseek		= retrode3_lseek,
	.read		= retrode3_scoped_read,
	.write		= retrode3_scoped_write,
	.unlocked_ioctl	= retrode3_scoped_ioctl,
	.open		= retrode3_scoped_open,
	.release	= retrode3_scoped_release,
};

// FIXME: duplicate to retrode3_release??

static void retrode3_slot_release(struct device *dev)
{
	struct retrode3_slot *slot = dev_get_drvdata(dev);

        dev_dbg(&slot->dev, "%s\n", __func__);

	if (slot->id >= 0)
		ida_free(&retrode3_minors, slot->id);
	kfree(slot);
}

int retrode3_probe_slot(struct retrode3_slot *slot, struct device_node *child)
{
	struct device *dev = &slot->dev;
	int id, ret;
	const char *name = "unknown";

	/* The slot is kzalloc'ed, and the final device reference frees it. */
	device_initialize(dev);
	dev->class = retrode3_class;
	dev->release = retrode3_slot_release;
	dev_set_drvdata(dev, slot);

	id = ida_alloc_max(&retrode3_minors, RETRODE3_MINORS - 1, GFP_KERNEL);
	if (id < 0) {
		ret = id;
		goto err_put_device;
	}
	slot->id = id;
	dev->devt = retrode3_first + id;

	of_property_read_string(child, "name", &name);
	ret = dev_set_name(dev, "slot-%s", name);
	if (ret)
		goto err_put_device;
	dev->of_node = child;

	slot->ce = devm_gpiod_get(dev, "ce", GPIOD_OUT_HIGH);
	if (IS_ERR(slot->ce)) {
		ret = PTR_ERR(slot->ce);
		goto err_put_device;
	}
	gpiod_set_value(slot->ce, 0);

	slot->cd = devm_gpiod_get(dev, "cd", GPIOD_IN);
	if (IS_ERR(slot->cd)) {
		ret = PTR_ERR(slot->cd);
		goto err_put_device;
	}
	slot->power = devm_gpiod_get(dev, "power", GPIOD_IN);
	of_property_read_u32_index(child, "address-width", 0, &slot->addr_width);
	of_property_read_u32_index(child, "bus-width", 0, &slot->bus_width);

	cdev_init(&slot->cdev, &slot_fops);
	slot->cdev.owner = THIS_MODULE;

	ret = cdev_device_add(&slot->cdev, dev);
	if (ret)
		goto err_put_device;

	if (!IS_ERR_OR_NULL(slot->power)) {
		if (set_slot_power_mV(slot, 3300) < 0)
			set_slot_power_mV(slot, 5000);
	}

	slot->cd_state = -1;
	atomic_set(&slot->cd_epoch, 0);
	/* status-led points to a gpio-leds class device, not a status GPIO. */
	if (of_property_present(child, "status-led")) {
		scnprintf(slot->status_led_trigger_name,
			 sizeof(slot->status_led_trigger_name), "retrode3-%s",
			 dev_name(dev));
		led_trigger_register_simple(slot->status_led_trigger_name,
					    &slot->status_led_trigger);
		if (!slot->status_led_trigger)
			dev_warn(dev, "cartridge LED trigger unavailable\n");
		else
			led_trigger_event(slot->status_led_trigger, LED_OFF);
	}
	INIT_DELAYED_WORK(&slot->work, retrode3_cd_work);

	return 0;

err_put_device:
	/* Also releases child devres, the minor and the slot allocation. */
	put_device(dev);
	return ret;
}

/*
 * cart detect and /sysfs
 */

static ssize_t sense_show(struct device *dev, struct device_attribute *attr,
				char *buf);

static void retrode3_update_cd(struct retrode3_slot *slot)
{
	int cd_state = gpiod_get_value(slot->cd);	// check cart detect pin

	if (cd_state < 0)
		return;	// ignore

	if (cd_state != slot-> cd_state) {
		char *envp[3];
		char buf[20];
		int len;

// printk("%s: state changed to %d\n", __func__, cd_state);

		slot->cd_state = cd_state;
		atomic_inc(&slot->cd_epoch);

		led_trigger_event(slot->status_led_trigger,
				  cd_state ? LED_FULL : LED_OFF);

		if (slot->bus_width == 16 && cd_state == 0)
			set_slot_power_mV(slot, 3300);	// turn off 5V mode (for MD slot)

		envp[0] = kasprintf(GFP_KERNEL, "SLOT=%s", dev_name(&slot->dev));
		len = sense_show(&slot->dev, NULL, buf);
		if (len < 0)
			return;
		if (buf[len - 1] == '\n')
			buf[len - 1] = 0;	// strip off \n
		envp[1] = kasprintf(GFP_KERNEL, "SENSE=%s", buf);
		envp[2] = NULL;
// printk("%s: %s %s\n", __func__, envp[0], envp[1]);
		// check with: udevadm monitor --environment
		kobject_uevent_env(&slot->dev.kobj, KOBJ_CHANGE, envp);

	}
}

#if 0
static irqreturn_t retrode3_gpio_cd_irqt(int irq, void *dev_id)
{
	struct retrode3_slot *slot = dev_id;

	retrode3_update_cd(slot);

	return IRQ_HANDLED;
}
#endif

static void retrode3_cd_work(struct work_struct *work)
{
	struct retrode3_slot *slot = container_of(work, struct retrode3_slot, work.work);

	retrode3_update_cd(slot);

	schedule_delayed_work(&slot->work,
			msecs_to_jiffies(50));	// start next check
}

#if FIXME

static const struct retrode3_device_id retrode3_slot_idtable[] = {
	{ "retrode3-slot", 0 },
	{ }
};

MODULE_DEVICE_TABLE(i2c, retrode3_slot_idtable);

static const struct of_device_id retrode3_slot_of_match[] = {
	{ .compatible = "openpandora,retrode3-slot" },
/* not yet used */
	{ .compatible = "openpandora,retrode3-slot-snes", .data = (void *) DEVICE_SNES },
	{ .compatible = "openpandora,retrode3-slot-megadrive", .data = (void *) DEVICE_MD },
	{ .compatible = "openpandora,retrode3-slot-nes", .data = (void *) DEVICE_NES },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, retrode3_slot_of_match);

static struct retrode3_driver retrode3_slot_driver = {
	.driver = {
		.name	= "retrode3-controller",
		.of_match_table = retrode3_slot_of_match,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.id_table	= retrode3_slot_idtable,
	.probe		= retrode3_probe_slot,
};

module_retrode3_driver(retrode3_slot_driver);

MODULE_AUTHOR("H. Nikolaus Schaller <hns@goldelico.com>");
MODULE_DESCRIPTION("Retrode3 Game Cartridge Slot Driver");
MODULE_LICENSE("GPL");

#endif

#undef set_address

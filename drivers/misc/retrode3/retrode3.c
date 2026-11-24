// SPDX-License-Identifier: GPL-2.0

/*
 * based on ideas and fragments from
 *  drivers/char/mem.c
 *  drivers/gnss/core.c
 *  arch/arm/common/locomo.c

- 8 bit vs. 16 bit bus width
- add read/write for RAM

Idee:
	aufsplitten in BUS openpandora,retrode3 (wie i2c-core)
	und 2 Arten Device-Treiber on demand laden
	openpandora,retrode3-slot
	openpandora,retrode3-controller
	d.h. einfach über "compatible"

dazu retrode3_device mit Pointer auf den bus


 *
 *  Copyright (C) 2022-26, H. Nikolaus Schaller
 *
 */

#include "retrode3_bus.h"
#include <linux/delay.h>
#include <linux/regmap.h>
#include <linux/string.h>

/*
 * TEMPORARY BENCH TEST, disabled by default.  The shared CLI can switch
 * the parameter for a single serialized probe/read and restore its value.
 * It is intentionally writable by root only; never enable from web input.
 * This may avoid opposing
 * GPIO outputs when a passive Retrode2 adapter bridges MD control pins.
 * High-Z does not electrically isolate a populated second cartridge.
 */
static bool legacy_bridge_hiz_test;
module_param_named(legacy_bridge_hiz_test, legacy_bridge_hiz_test, bool, 0644);
MODULE_PARM_DESC(legacy_bridge_hiz_test,
	"BENCH ONLY: MD CE high-Z during SNES/fixed-lane adapter reads, no writes");

/* Enabled by the trusted CLI's scoped hardware profile for SMS reads only.
 * The kernel still independently restricts addresses, values, slot and 5V.
 * Runtime switching must happen under the shared userspace cartridge lock. */
static bool sms_mapper_write_test;
module_param_named(sms_mapper_write_test, sms_mapper_write_test, bool, 0644);
MODULE_PARM_DESC(sms_mapper_write_test,
	"BENCH ONLY: guarded SMS $FFFC-$FFFF mapper writes at 5V with high-Z");

/* Scoped GBx ROM profile. The common CLI owns the inter-process bus lock.
 * The kernel also serializes transitions against file operations, controller
 * polling and raw sysfs IO. No direction change while a GBx FD is open.
 */
static bool gbx_md_test;
static DEFINE_MUTEX(gbx_profile_lock);
static struct retrode3_bus *gbx_profile_bus;
static unsigned int gbx_profile_users;
static int retrode3_gbx_param_set(const char *val, const struct kernel_param *kp);
static const struct kernel_param_ops retrode3_gbx_param_ops = {
	.set = retrode3_gbx_param_set,
	.get = param_get_bool,
};
module_param_cb(gbx_md_test, &retrode3_gbx_param_ops, &gbx_md_test, 0644);
MODULE_PARM_DESC(gbx_md_test,
	"Scoped GBx-0011 ROM-only MD bus profile; restore on disable");
/*
 * Hardware-guarded ROM-read experiment: one GPIOA PIN snapshot can
 * replace 8 or 16 per-bit GPIO reads on the X1600.
 * Default ON after successful native MD/SNES/NES and SMS hardware tests.
 * Independent from the verified Flash write fast path.
 */
static bool r3_rom_read_bulk_gpio = true;
module_param_named(rom_read_bulk_gpio, r3_rom_read_bulk_gpio, bool, 0600);
MODULE_PARM_DESC(rom_read_bulk_gpio,
	"X1600 PA8..PA23 single-register ROM reads (default on; 0=per-pin fallback)");

/* Independently configurable ROM-read optimizations. */
static bool r3_rom_addr_bulk_gpio = true;
module_param_named(rom_addr_bulk_gpio, r3_rom_addr_bulk_gpio, bool, 0600);
MODULE_PARM_DESC(rom_addr_bulk_gpio,
	"X1600 native MD/SNES ROM address bank SET/CLEAR (default on)");

static bool r3_rom_snes_oe_hold = true;
module_param_named(rom_snes_oe_hold, r3_rom_snes_oe_hold, bool, 0600);
MODULE_PARM_DESC(rom_snes_oe_hold,
	"SNES native ROM: hold /OE within each bounded read buffer (default on)");

#define R3_X1600_GPIOA_PIN_REG 0x00
#define R3_ROM_GPIO_BANK_STRIDE 0x100
#define R3_ROM_PAT0_SET 0x44
#define R3_ROM_PAT0_CLEAR 0x48

/*
 * Determine mapping ONCE at probe. Require all 16 active-high GPIOA
 * descriptors in D0..D15 == PA8..PA23 order on an X1600 controller.
 * The Ingenic pinctrl parent owns its regmap. This driver only reads
 * the input GPIO_PIN register, not output values or pin directions.
 */
static void retrode3_probe_bulk_rom_reader(struct device *dev,
					    struct retrode3_bus *bus)
{
	struct gpio_descs *datas = bus->datas;
	struct gpio_chip *gc;
	struct regmap *map;
	int i;

	if (!datas || datas->ndescs != 16)
		return;

	gc = datas->desc[0]->gdev->chip;
	if (!gc || !gc->parent || !gc->parent->of_node ||
	    !of_device_is_compatible(gc->parent->of_node,
				     "ingenic,x1600-pinctrl") ||
	    !gc->label || strcmp(gc->label, "GPIOA") ||
	    gc->ngpio != 32 || !gc->get)
		return;

	for (i = 0; i < 16; i++) {
		struct gpio_desc *desc = datas->desc[i];

		if (!desc || desc->gdev->chip != gc ||
		    gpiod_hwgpio(desc) != 8 + i ||
		    gpiod_is_active_low(desc))
			return;
	}

	map = dev_get_drvdata(gc->parent);
	if (!map)
		return;

	bus->rom_read_bank_map = map;
	dev_info(dev,
		 "X1600 GPIOA PA8..PA23 bulk ROM read available (rom_read_bulk_gpio=%u)\n",
		 !!READ_ONCE(r3_rom_read_bulk_gpio));
}

/*
 * Only the exact native Retrode3 2.9.x X1600 address layout is accepted.
 * Validate against actual descriptors, not just the board name. Older
 * boards with A0 on PB30 and any unknown topology use the legacy path.
 * PAT0 aliases affect only the requested address bits, never PA8..23
 * data or any unrelated pins. The shared pinctrl regmap owns the bank.
 */
static void retrode3_probe_bulk_rom_address(struct device *dev,
					    struct retrode3_bus *bus)
{
	struct gpio_chip *ga, *gb;
	unsigned int i;

	if (!bus->rom_read_bank_map || !bus->addrs ||
	    bus->addrs->ndescs != 24)
		return;
	ga = bus->addrs->desc[0]->gdev->chip;
	gb = bus->addrs->desc[16]->gdev->chip;
	if (!ga || !gb || !ga->parent || ga->parent != gb->parent ||
	    !ga->label || strcmp(ga->label, "GPIOA") ||
	    !gb->label || strcmp(gb->label, "GPIOB") ||
	    ga->ngpio != 32 || gb->ngpio != 32 ||
	    dev_get_drvdata(ga->parent) != bus->rom_read_bank_map)
		return;

	for (i = 0; i < 24; i++) {
		struct gpio_desc *desc = bus->addrs->desc[i];
		unsigned int pin = i < 8 ? i : i < 16 ? i + 16 :
				   i < 22 ? i - 4 : i - 3;

		if (!desc || desc->gdev->chip != (i < 16 ? ga : gb) ||
		    gpiod_hwgpio(desc) != pin || gpiod_is_active_low(desc))
			return;
	}
	bus->rom_addr_bulk_available = true;
	dev_info(dev, "X1600 PA/PB bulk ROM address available (rom_addr_bulk_gpio=%u)\n",
		 !!READ_ONCE(r3_rom_addr_bulk_gpio));
}

/* GPIO_PIN is offset 0 in bank A for this exact X1600 pinctrl. */
static inline int retrode3_bulk_rom_word(struct retrode3_bus *bus)
{
	unsigned int bank;
	int err;

	err = regmap_read(bus->rom_read_bank_map, R3_X1600_GPIOA_PIN_REG,
			  &bank);
	if (err)
		return err;
	return (bank >> 8) & 0xffff;
}

/* low level address and data bus access */

// FXIME: get_multiple / set_multiple could be much faster, if supported by device driver

static inline int get_bus_bit(struct gpio_desc *desc)
{
#if 1
	struct gpio_chip *gc = desc->gdev->chip;
	return gc->get(gc, gpiod_hwgpio(desc));
#else
	return gpiod_get_value(desc);
#endif

}

static inline int set_bus_bit(struct gpio_desc *desc, int value)
{
#if 1	// speed optimized direct call
	struct gpio_chip *gc = desc->gdev->chip;
	// can we use set_multiple?
// printk("%s: %px set=%pS set=%pS\n", __func__, gc, gc->set, gc->set);
	return gc->set(gc, gpiod_hwgpio(desc), value);
#else
	return gpiod_set_value(desc, value);
#endif
}

/* access to cart bus */

static inline int set_address(struct retrode3_bus *bus, uint32_t addr)
{
	int a, err;
	u32 changed;

	if (addr >= EOF)
		return -EINVAL;
	changed = bus->rom_addr_cache_dirty ? (u32)(EOF - 1) :
					     addr ^ bus->current_addr;
	if (!changed)
		return 0;

	for (a = 0; a < bus->addrs->ndescs; a++) {
		if (changed & BIT(a)) {
			err = set_bus_bit(bus->addrs->desc[a], (addr >> a) & 1);
			if (err) {
				bus->rom_addr_cache_dirty = true;
				return err;
			}
		}
	}
	bus->current_addr = addr;
	bus->rom_addr_cache_dirty = false;
	return 0;
}

static inline u32 retrode3_rom_address_pa(u32 addr)
{
	return (addr & 0xffU) | ((addr & 0xff00U) << 16);
}

static inline u32 retrode3_rom_address_pb(u32 addr)
{
	return ((addr & 0x3f0000U) >> 4) | ((addr & 0xc00000U) >> 3);
}

static inline int retrode3_rom_address_bank(struct regmap *map,
					   unsigned int base, u32 changed,
					   u32 value)
{
	u32 clear = changed & ~value;
	u32 set = changed & value;
	int err;

	if (clear) {
		err = regmap_write(map, base + R3_ROM_PAT0_CLEAR, clear);
		if (err)
			return err;
	}
	if (set)
		return regmap_write(map, base + R3_ROM_PAT0_SET, set);
	return 0;
}

/* Native ROM reads only. No GBx, SMS, NES, Flash command or FRAM modes. */
static inline int retrode3_set_rom_address(struct retrode3_bus *bus,
					    u32 addr, bool bulk)
{
	u32 changed;
	int err;

	if (!bulk)
		return set_address(bus, addr);
	if (addr >= EOF)
		return -EINVAL;
	changed = bus->rom_addr_cache_dirty ? (u32)(EOF - 1) :
					     addr ^ bus->current_addr;
	if (!changed)
		return 0;

	err = retrode3_rom_address_bank(bus->rom_read_bank_map, 0,
				      retrode3_rom_address_pa(changed),
				      retrode3_rom_address_pa(addr));
	if (!err && (changed & 0xff0000U))
		err = retrode3_rom_address_bank(bus->rom_read_bank_map,
					      R3_ROM_GPIO_BANK_STRIDE,
					      retrode3_rom_address_pb(changed),
					      retrode3_rom_address_pb(addr));
	if (err) {
		/* A partially applied bank update must never poison the cache. */
		bus->rom_addr_cache_dirty = true;
		return err;
	}
	bus->current_addr = addr;
	bus->rom_addr_cache_dirty = false;
	return 0;
}

// FIXME: force to switch direction of D lines to input? Or rely on write turning them back?

static inline int get_half(struct retrode3_bus *bus, int a0)
{ /* read data from data lines D0..D7 (a0 = 1) or D8..D15 (a0 = 0) */
	int d;
	uint8_t data;

	if (READ_ONCE(r3_rom_read_bulk_gpio) && bus->rom_read_bank_map) {
		int word = retrode3_bulk_rom_word(bus);

		if (word < 0)
			return word;
		return a0 ? (word & 0xff) : ((word >> 8) & 0xff);
	}
// printk("%s: a0=%d\n", __func__, a0);

	/* read 8 data bits either on D0..D7 (a0 = 1) or D8..D15 (a0 = 0) */
	data = 0;
	if(a0)
		for (d = 0; d < bus->datas->ndescs-8; d++) {
			int bit = get_bus_bit(bus->datas->desc[d]);
// printk("%s: bit %d=%d\n", __func__, d, bit);
			if (bit < 0)
				return bit;
			data |= bit << d;
		}
	else
		for (d = 8; d < bus->datas->ndescs; d++) {
			int bit = get_bus_bit(bus->datas->desc[d]);
// printk("%s: bit %d=%d\n", __func__, d, bit);
			if (bit < 0)
				return bit;
			data |= bit << (d-8);
		}

// printk("%s: data=%02x\n", __func__, data);

	return data;
}

static inline int read_half(struct retrode3_bus *bus, int a0)
{ /* read data from data lines D0..D7 (a0 = 1) or D8..D15 (a0 = 0) with OE */
	int data; /* Do not silently truncate a GPIO read error. */

	set_bus_bit(bus->oe, 0);	// this is the pin level although we have defined "active low" in the DTS
	/* Let ROM outputs and level shifters settle before sampling data. */
	udelay(1);
	data = get_half(bus, a0);
	set_bus_bit(bus->oe, 1);

	return data;
}

static inline int read_byte(struct retrode3_bus *bus)
{ /* read data from data lines */
	return read_half(bus, bus->current_addr & 1);
}

static inline int get_word(struct retrode3_bus *bus)
{ /* read data from data lines */
	int d;
	uint16_t data;

	if (READ_ONCE(r3_rom_read_bulk_gpio) && bus->rom_read_bank_map)
		return retrode3_bulk_rom_word(bus);

// printk("%s:\n", __func__);

	/* read 16 data bits */
	data = 0;
	for (d = 0; d < bus->datas->ndescs; d++) {
		int bit = get_bus_bit(bus->datas->desc[d]);

// printk("bit=%d\n", bit);

		if (bit < 0)
			return bit;
		data |= bit << d;
	}

// printk("%s: data=%02x\n", __func__, data);

	return data;
}

static inline int read_word(struct retrode3_bus *bus)
{ /* read data from data lines */
	int data;

	set_bus_bit(bus->oe, 0);	// this is the pin level although we have "active low"
	/* Let ROM outputs and level shifters settle before sampling data. */
	udelay(1);
	data = get_word(bus);
	set_bus_bit(bus->oe, 1);

	return data;
}

static inline void set_half(struct retrode3_bus *bus, uint8_t data, int a0)
{ /* set D0..D7 (a0 = 1) or D8..D15 (a0 = 0) */
	int d;
	/* set data bits */

// printk("%s:\n", __func__);

	if(a0) { // D0..D7
		for (d = 0; d < bus->datas->ndescs-8; d++) {
			gpiod_direction_output(bus->datas->desc[d], (data>>d) & 1);
		}
	} else { // D8..D15
		for (d = 8; d < bus->datas->ndescs; d++) {
			gpiod_direction_output(bus->datas->desc[d], (data>>(d-8)) & 1);
		}
	}
}

static inline void drive_half(struct retrode3_bus *bus, uint8_t data, int a0)
{ // write D0..D7 (a0 = 1) or D8..D15 (a0 = 0) with WE0 or WE8
	set_half(bus, data, a0);
	/* pulse write enable depending on a0 (WE8 for a0 = 1, WE0 for a0 = 0) */
	gpiod_set_value(bus->we->desc[a0?0:1], 1);
	gpiod_set_value(bus->we->desc[a0?0:1], 0);
}

static inline void end_drive_word(struct retrode3_bus *bus)
{ /* switch data bus back to input */
	int d;
	for (d = 0; d < bus->datas->ndescs; d++) {
		gpiod_direction_input(bus->datas->desc[d]);
	}
}

static inline void write_half(struct retrode3_bus *bus, uint8_t data, int a0)
{ // write D0..D7 (a0 = 1) or D8..D15 (a0 = 0) with WE0 or WE8 and switch data bus back to input
	drive_half(bus, data, a0);
	end_drive_word(bus);
}

static inline void write_byte(struct retrode3_bus *bus, uint8_t data)
{ // use a0 of current_address
	return write_half(bus, data, bus->current_addr & 1);
}

static inline void set_word(struct retrode3_bus *bus, uint16_t data)	// D0..D15
{ /* set D0..D15 */
	int d;
	/* set data bits */

// printk("%s:\n", __func__);

	for (d = 0; d < bus->datas->ndescs; d++) {
		gpiod_direction_output(bus->datas->desc[d], (data>>d) & 1);
	}
}

static inline void drive_word(struct retrode3_bus *bus, uint16_t data)	// D0..D15
{ /* write data to data lines */
	set_word(bus, data);
	/* pulse both write enables */
	gpiod_set_value(bus->we->desc[0], 1);
	gpiod_set_value(bus->we->desc[1], 1);
	gpiod_set_value(bus->we->desc[0], 0);
	gpiod_set_value(bus->we->desc[1], 0);
}

static inline void write_word(struct retrode3_bus *bus, uint16_t data)	// D0..D15
{
	drive_word(bus, data);
	end_drive_word(bus);
}

#define CONFIG_RETRODE3_MDSLOT 294

static int get_slot_power_mV(struct retrode3_slot *slot)
{
	if(!slot || IS_ERR_OR_NULL(slot->power))
		return -ENODEV;
	return gpiod_get_direction(slot->power) ? 3300 : 5000;
}

static int set_slot_power_mV(struct retrode3_slot *slot, int mV)
{
printk("%s: %dmV %px\n", __func__, mV, slot->power);

	if (IS_ERR_OR_NULL(slot->power))
		return -ENODEV;

	switch(mV) {
		case 5000:
			gpiod_direction_output(slot->power, 0);	// switch to output 0
			break;
		case 3300:
#if CONFIG_RETRODE3_MDSLOT == 293
			return -EINVAL;	// broken
#endif
			gpiod_direction_input(slot->power);	// switch to floating
			break;
		default:
printk("%s: unknown voltage %dmV\n", __func__, mV);
			return -EINVAL;
	}
	return 0;
}

static int is_selected(struct retrode3_slot *slot)
{
	// errors are treated as selected...
	return gpiod_get_value(slot->ce);
}

static void select_slot(struct retrode3_bus *bus, struct retrode3_slot *slot)
{ /* control cart select */
	int i;

// printk("%s %d: %px\n", __func__, __LINE__, slot);

	if (slot) {
		if (is_selected(slot))
			return;	// already selected (and locked)
		mutex_lock(&bus->select_lock);	// lock until slot == NULL
	}

	/* make sure that only one or no slot is selected at any time */
	for(i=0; i<ARRAY_SIZE(bus->slots); i++) {
		if (!bus->slots[i])
			continue;	// avoid to match a slot without ce
		gpiod_set_value(bus->slots[i]->ce, (bus->slots[i] == slot) ? 1:0);
	}

	if (!slot && mutex_is_locked(&bus->select_lock))
		mutex_unlock(&bus->select_lock);
}

/* Si5351 clocks */
static int set_frequency(struct retrode3_bus *bus, int channel, unsigned long frequency)
{
	if (channel < 0 || channel >= ARRAY_SIZE(bus->clks) || !bus->clks[channel])
		return -EINVAL;

	if (frequency == 0) {
		if (__clk_is_enabled(bus->clks[channel])) {
			clk_disable_unprepare(bus->clks[channel]);
			pr_info("Channel %d physically disabled\n", channel);
		}
	} else {
		int ret = clk_set_rate(bus->clks[channel], frequency);
		if (ret) {
			pr_info("Channel %d: Failed to set rate to %lu Hz\n", channel, frequency);
			return ret;
		}

		if (!__clk_is_enabled(bus->clks[channel])) {
			ret = clk_prepare_enable(bus->clks[channel]);
			if (ret) {
				pr_info("Channel %d: Failed to enable clock\n", channel);
				return ret;
			}
			pr_info("Channel %d powered up to %lu Hz\n", channel, clk_get_rate(bus->clks[channel]));
		} else {
			pr_info("Channel %d frequency changed to %lu Hz\n", channel, clk_get_rate(bus->clks[channel]));
		}
	}

	return 0;
}

static unsigned long get_frequency(struct retrode3_bus *bus, int channel)
{
	if (channel < 0 || channel >= ARRAY_SIZE(bus->clks) || !bus->clks[channel])
		return -EINVAL;

	if (!__clk_is_enabled(bus->clks[channel]))
		return 0;	// disabled

	return clk_get_rate(bus->clks[channel]);
}

// FIXME: make this kernel modules loaded and linked at runtime by retrode3_probe

#include "retrode3_cart.c"
#include "retrode3_controller.c"

/* Called with the profile mutex held; no file or raw GPIO operation can
 * start during the transition. The ordinary select mutex also excludes
 * an outstanding legacy sysfs-selected slot.
 */
static int retrode3_gbx_param_set(const char *val, const struct kernel_param *kp)
{
	struct retrode3_bus *bus;
	bool enable;
	int err;

	err = kstrtobool(val, &enable);
	if (err)
		return err;
	mutex_lock(&gbx_profile_lock);
	bus = gbx_profile_bus;
	if (enable == gbx_md_test && (enable || !bus || !bus->gbx_active))
		goto done;
	if (gbx_profile_users) {
		err = -EBUSY;
		goto done;
	}
	if (bus) {
		if (!mutex_trylock(&bus->select_lock)) {
			err = -EBUSY;
			goto done;
		}
		if (!enable && bus->gbx_active)
			err = retrode3_gbx_restore(bus);
		mutex_unlock(&bus->select_lock);
		if (err)
			goto done;
	}
	gbx_md_test = enable;
done:
	mutex_unlock(&gbx_profile_lock);
	return err;
}

/* Stop workers first: neither GPIOs nor child devices may be released while
 * a cartridge-detect or game-controller poll is still running.
 */
static void retrode3_unregister_slots(struct retrode3_bus *bus)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(bus->slots); i++) {
		if (bus->slots[i])
			cancel_delayed_work_sync(&bus->slots[i]->work);
	}

	select_slot(bus, NULL);

	for (i = 0; i < ARRAY_SIZE(bus->slots); i++) {
		struct retrode3_slot *slot = bus->slots[i];

		if (!slot)
			continue;

		if (slot->is_controller) {
			device_unregister(&slot->dev);
		} else {
			/* Polling has stopped; detach the LED trigger before slot release. */
			led_trigger_unregister_simple(slot->status_led_trigger);
			cdev_device_del(&slot->cdev, &slot->dev);
			put_device(&slot->dev);
		}
		bus->slots[i] = NULL;
	}
}

/* bus driver probe */

static int retrode3_probe(struct platform_device *pdev)
{
        struct retrode3_bus *bus;
        int i, ret;
	struct device_node *slots, *child = NULL;

        dev_dbg(&pdev->dev, "%s\n", __func__);

        if (!pdev->dev.of_node) {
                dev_err(&pdev->dev, "No device tree data\n");
                return EINVAL;
        }

        bus = devm_kzalloc(&pdev->dev, sizeof(*bus), GFP_KERNEL);
        if (bus == NULL)
                return -ENOMEM;

// printk("%s: a bus=%px\n", __func__, bus);

	bus->addrs = devm_gpiod_get_array(&pdev->dev, "addr", GPIOD_OUT_HIGH);
// printk("%s: addrs=%px\n", __func__, bus->addrs);
	if (IS_ERR(bus->addrs))
		return PTR_ERR(bus->addrs);
	if (bus->addrs->ndescs != 24) {
		dev_err(&pdev->dev, "Invalid number of address gpios (%d != 24)\n",
			bus->addrs->ndescs);
		return -EINVAL;
	}
// printk("%s: addrs 1\n", __func__);
	/* bring all address gpios in a defined state */
	bus->current_addr = EOF - 1;
// printk("%s: addrs 2\n", __func__);
	set_address(bus, 0);
// printk("%s: addrs 3\n", __func__);

	bus->datas = devm_gpiod_get_array(&pdev->dev, "data", GPIOD_IN);
// printk("%s: datas=%px\n", __func__, bus->datas);
	if (IS_ERR(bus->datas))
		return PTR_ERR(bus-> datas);
	if (bus->datas->ndescs != 16) {
		dev_err(&pdev->dev, "Invalid number of data gpios (%d != 16)\n",
			bus->datas->ndescs);
		return -EINVAL;
	}

	/* Mapping is cached at probe; never rediscover it per word. */
	retrode3_probe_bulk_rom_reader(&pdev->dev, bus);
	retrode3_probe_bulk_rom_address(&pdev->dev, bus);

	bus->oe = devm_gpiod_get(&pdev->dev, "oe", GPIOD_OUT_HIGH);	// active LOW is XORed with DT definition
// printk("%s: oe=%px\n", __func__, bus->oe);
	if (IS_ERR(bus->oe))
		return PTR_ERR(bus->oe);
	gpiod_set_value(bus->oe, 0);	// turn inactive

	bus->we = devm_gpiod_get_array(&pdev->dev, "we", GPIOD_OUT_HIGH);	// active LOW is XORed with DT definition
// printk("%s: we=%px\n", __func__, bus->we);
	if (IS_ERR(bus->we))
		return PTR_ERR(bus->we);
	if (bus->we->ndescs != 2) {
		dev_err(&pdev->dev, "Invalid number of we gpios (%d != 2)\n",
			bus->we->ndescs);
		return -EINVAL;
	}
	gpiod_set_value(bus->we->desc[0], 0);	// make both inactive
	gpiod_set_value(bus->we->desc[1], 0);

	bus->time = devm_gpiod_get(&pdev->dev, "time", GPIOD_OUT_HIGH);	// active LOW is XORed with DT definition
// printk("%s: time=%px\n", __func__, bus->time);
	if (IS_ERR(bus->time))
		return PTR_ERR(bus->time);
	gpiod_set_value(bus->time, 0);	// make inactive

	bus->phi2 = devm_gpiod_get(&pdev->dev, "phi2", GPIOD_OUT_HIGH);
printk("%s: time=%px\n", __func__, bus->phi2);
	if (!IS_ERR(bus->phi2))
		gpiod_set_value(bus->phi2, 0);	// make inactive

	bus->reset = devm_gpiod_get(&pdev->dev, "reset", GPIOD_OUT_HIGH);	// active LOW is XORed with DT definition
// printk("%s: reset=%px\n", __func__, bus->reset);
	if (IS_ERR(bus->reset))
		return PTR_ERR(bus->reset);
	gpiod_set_value_cansleep(bus->reset, 0);	// make inactive

#if 0
printk("%s: 1\n", __func__);
printk("%s: bus=%px\n", __func__, bus);
printk("%s: addrs=%px\n", __func__, bus->addrs);
printk("%s: desc[0]=%px\n", __func__, bus->addrs->desc[0]);
printk("%s: gdev=%px\n", __func__, bus->addrs->desc[0]->gdev);
printk("%s: chip=%px\n", __func__, bus->addrs->desc[0]->gdev->chip);
#endif

#if 0
{
	struct gpio_chip *gc = bus->addrs->desc[0]->gdev->chip;
	printk("%s: get %ps\n", __func__, gc->get);
	printk("%s: getmult %ps\n", __func__, gc->get_multiple);
	printk("%s: set %ps\n", __func__, gc->set);
	printk("%s: setmult %ps\n", __func__, gc->set_multiple);
	printk("%s: dirout %ps\n", __func__, gc->direction_output);	// no direction_output_multiple!
	printk("%s: dirin %ps\n", __func__, gc->direction_input);	// no direction_input_multiple!
	printk("%s: setconf %ps\n", __func__, gc->set_config);
}
#endif

	for (i = 0; i < ARRAY_SIZE(bus->clks); i++) {
		char clk_name[12];
		snprintf(clk_name, sizeof(clk_name), "clk_out%d", i);
		bus->clks[i] = devm_clk_get(&pdev->dev, clk_name);
		if (IS_ERR(bus->clks[i])) {
			dev_err(&pdev->dev, "Failed to resolve %s from DT\n", clk_name);
			return PTR_ERR(bus->clks[i]);
		}
	}

	mutex_init(&bus->select_lock);

// FIXME: do we needs this grouping?

	slots = of_get_child_by_name(pdev->dev.of_node, "slots");
	if (!slots) {
		ret = -ENODEV;
		goto err_destroy_mutex;
	}

	i = 0;
	while ((child = of_get_next_child(slots, child))) {
		struct retrode3_slot *slot;

		if (i >= ARRAY_SIZE(bus->slots)) {
			dev_err(&pdev->dev, "too many slots\n");
			ret = -EINVAL;
			goto err_unregister_slots;
		}

		/* Child device lifetime is controlled by its release callback,
		 * not by the platform device's managed allocations.
		 */
		slot = kzalloc(sizeof(*slot), GFP_KERNEL);
		if (!slot) {
			ret = -ENOMEM;
			goto err_unregister_slots;
		}

		slot->id = -1;
		slot->bus = bus;
		slot->dev.parent = &pdev->dev;

		if (of_property_match_string(child, "compatible",
					     "openpandora,retrode3-slot") >= 0) {
			ret = retrode3_probe_slot(slot, child);
		} else if (of_property_match_string(child, "compatible",
						    "openpandora,retrode3-controller") >= 0) {
			slot->is_controller = true;
			ret = retrode3_probe_controller(slot, child);
		} else {
			dev_err(&pdev->dev, "unknown child type\n");
			kfree(slot);
			ret = -EINVAL;
			goto err_unregister_slots;
		}

		/* Both child probe functions release slot on failure. */
		if (ret < 0)
			goto err_unregister_slots;

		bus->slots[i++] = slot;
	}
	of_node_put(slots);

	select_slot(bus, NULL);
	platform_set_drvdata(pdev, bus);
	mutex_lock(&gbx_profile_lock);
	gbx_profile_bus = bus;
	mutex_unlock(&gbx_profile_lock);

	/* Start polling only after every slot has been fully registered. */
	for (i = 0; i < ARRAY_SIZE(bus->slots); i++) {
		struct retrode3_slot *slot = bus->slots[i];

		if (!slot)
			continue;
		schedule_delayed_work(&slot->work, slot->is_controller ?
				      POLL_RATE : msecs_to_jiffies(50));
	}
	dev_dbg(&pdev->dev, "%s successful\n", __func__);
	return 0;

err_unregister_slots:
	of_node_put(child);
	of_node_put(slots);
	retrode3_unregister_slots(bus);
err_destroy_mutex:
	mutex_destroy(&bus->select_lock);
	return ret;
}

static void retrode3_remove(struct platform_device *pdev)
{
	struct retrode3_bus *bus = platform_get_drvdata(pdev);
	int i;

	mutex_lock(&gbx_profile_lock);
	if (gbx_profile_bus == bus)
		gbx_profile_bus = NULL;
	mutex_unlock(&gbx_profile_lock);
	retrode3_unregister_slots(bus);

	for (i = 0; i < ARRAY_SIZE(bus->clks); i++) {
		if (bus->clks[i] && __clk_is_enabled(bus->clks[i]))
			clk_disable_unprepare(bus->clks[i]);
	}

	mutex_destroy(&bus->select_lock);
}

static const struct of_device_id retrode3_of_match[] = {
        { .compatible = "openpandora,retrode3" },
        {},
};
MODULE_DEVICE_TABLE(of, retrode3_of_match);

static struct platform_driver retrode3_driver = {
        .probe          = retrode3_probe,
        .remove         = retrode3_remove,
        .driver = {
                .name   = "retrode3",
                .owner  = THIS_MODULE,
                .of_match_table = of_match_ptr(retrode3_of_match)
        },
};


static ssize_t sense_show(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	struct retrode3_slot *slot = dev_get_drvdata(dev);

	if (!slot || IS_ERR_OR_NULL(slot->cd))
		return sprintf(buf, "%s\n", "unknown");

// sould use sysfs_emit()
	return sprintf(buf, "%s\n", gpiod_get_value(slot->cd)?"active":"empty");
}
static DEVICE_ATTR_RO(sense);

static ssize_t vcc_show(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	struct retrode3_slot *slot = dev_get_drvdata(dev);
	int mV = get_slot_power_mV(slot);

	if (mV < 0)
		return sprintf(buf, "fixed\n");

	return sprintf(buf, "%dmV\n", mV);
}

static ssize_t vcc_store_raw(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct retrode3_slot *slot = dev_get_drvdata(dev);
	unsigned int mV;
	int err;

	if(IS_ERR_OR_NULL(slot->power))
		return -ENODEV;	// can't control

	err = kstrtouint(buf, 10, &mV);
	if (err < 0)
		return err;

	err = set_slot_power_mV(slot, mV);
	if (err < 0)
		return err;

	return count;
}

static ssize_t vcc_store(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t count)
{
	ssize_t ret;

	if (!mutex_trylock(&gbx_profile_lock))
		return -EBUSY;
	if (gbx_profile_users)
		ret = -EBUSY;
	else
		ret = vcc_store_raw(dev, attr, buf, count);
	mutex_unlock(&gbx_profile_lock);
	return ret;
}

static DEVICE_ATTR_RW(vcc);

static ssize_t addr_show_raw(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	struct retrode3_slot *slot = dev_get_drvdata(dev);

	if (!is_selected(slot))
		return sprintf(buf, "deselected\n");

	return sprintf(buf, "%08x\n", slot->bus->current_addr);
}

static ssize_t addr_show(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	ssize_t ret;

	if (!mutex_trylock(&gbx_profile_lock))
		return -EBUSY;
	if ((gbx_md_test || (gbx_profile_bus && gbx_profile_bus->gbx_active)))
		ret = -EBUSY;
	else
		ret = addr_show_raw(dev, attr, buf);
	mutex_unlock(&gbx_profile_lock);
	return ret;
}


static ssize_t addr_store_raw(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct retrode3_slot *slot = dev_get_drvdata(dev);
	uint32_t addr;
	int err;

	if (strncmp(buf, "deselect", 8) == 0) {
		end_drive_word(slot->bus);
		select_slot(slot->bus, NULL);
		return count;
	}

	if (!is_selected(slot))
		select_slot(slot->bus, NULL);	// auto-deselect other slot

	// check for 0x prefix?
	err = kstrtouint(buf, 16, &addr);
	if (err < 0)
		return err;

	select_slot(slot->bus, slot);
	err = set_address(slot->bus, addr);
	if (err < 0)
		return err;

	return count;
}

static ssize_t addr_store(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t count)
{
	ssize_t ret;

	if (!mutex_trylock(&gbx_profile_lock))
		return -EBUSY;
	if ((gbx_md_test || (gbx_profile_bus && gbx_profile_bus->gbx_active)))
		ret = -EBUSY;
	else
		ret = addr_store_raw(dev, attr, buf, count);
	mutex_unlock(&gbx_profile_lock);
	return ret;
}

static DEVICE_ATTR_RW(addr);

static ssize_t data8_show_raw(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	struct retrode3_slot *slot = dev_get_drvdata(dev);
	int byte;

	if (!is_selected(slot))
		return sprintf(buf, "deselected\n");

	byte = read_byte(slot->bus);

	if (byte < 0)
		return byte;

	return sprintf(buf, "%02x\n", byte);
}

static ssize_t data8_show(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	ssize_t ret;

	if (!mutex_trylock(&gbx_profile_lock))
		return -EBUSY;
	if ((gbx_md_test || (gbx_profile_bus && gbx_profile_bus->gbx_active)))
		ret = -EBUSY;
	else
		ret = data8_show_raw(dev, attr, buf);
	mutex_unlock(&gbx_profile_lock);
	return ret;
}


static ssize_t data8_store_raw(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct retrode3_slot *slot = dev_get_drvdata(dev);
	unsigned int value;
	int err;

	if (!is_selected(slot))
		return -EACCES;

	err = kstrtouint(buf, 16, &value);
	if (err < 0)
		return err;

	if (value > 0x0ff)
		return -EINVAL;

	/* leave bus active */
	drive_half(slot->bus, value, slot->bus->current_addr & 1);

	return count;
}

static ssize_t data8_store(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t count)
{
	ssize_t ret;

	if (!mutex_trylock(&gbx_profile_lock))
		return -EBUSY;
	if ((gbx_md_test || (gbx_profile_bus && gbx_profile_bus->gbx_active)))
		ret = -EBUSY;
	else
		ret = data8_store_raw(dev, attr, buf, count);
	mutex_unlock(&gbx_profile_lock);
	return ret;
}

static DEVICE_ATTR_RW(data8);

static ssize_t data16_show_raw(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	struct retrode3_slot *slot = dev_get_drvdata(dev);
	int word;

	if (!is_selected(slot))
		return sprintf(buf, "deselected\n");

	word = read_word(slot->bus);

	if (word < 0)
		return word;

	return sprintf(buf, "%04x\n", word);
}

static ssize_t data16_show(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	ssize_t ret;

	if (!mutex_trylock(&gbx_profile_lock))
		return -EBUSY;
	if ((gbx_md_test || (gbx_profile_bus && gbx_profile_bus->gbx_active)))
		ret = -EBUSY;
	else
		ret = data16_show_raw(dev, attr, buf);
	mutex_unlock(&gbx_profile_lock);
	return ret;
}


static ssize_t data16_store_raw(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct retrode3_slot *slot = dev_get_drvdata(dev);
	unsigned int value;
	int err;

	if (!is_selected(slot))
		return -EACCES;

	err = kstrtouint(buf, 16, &value);
	if (err < 0)
		return err;

	if (value > 0x0ffff)
		return -EINVAL;

	/* leave bus active */
	drive_word(slot->bus, value);

	return count;
}

static ssize_t data16_store(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t count)
{
	ssize_t ret;

	if (!mutex_trylock(&gbx_profile_lock))
		return -EBUSY;
	if ((gbx_md_test || (gbx_profile_bus && gbx_profile_bus->gbx_active)))
		ret = -EBUSY;
	else
		ret = data16_store_raw(dev, attr, buf, count);
	mutex_unlock(&gbx_profile_lock);
	return ret;
}

static DEVICE_ATTR_RW(data16);

static ssize_t raw16_show_raw(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	struct retrode3_slot *slot = dev_get_drvdata(dev);
	int word;

	word = get_word(slot->bus);

	if (word < 0)
		return word;

	return sprintf(buf, "%04x\n", word);
}

static ssize_t raw16_show(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	ssize_t ret;

	if (!mutex_trylock(&gbx_profile_lock))
		return -EBUSY;
	if ((gbx_md_test || (gbx_profile_bus && gbx_profile_bus->gbx_active)))
		ret = -EBUSY;
	else
		ret = raw16_show_raw(dev, attr, buf);
	mutex_unlock(&gbx_profile_lock);
	return ret;
}


static ssize_t raw16_store_raw(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct retrode3_slot *slot = dev_get_drvdata(dev);
	unsigned int value;
	int err;

	err = kstrtouint(buf, 16, &value);
	if (err < 0)
		return err;

	if (value > 0x0ffff)
		return -EINVAL;

	/* leave bus active */
	set_word(slot->bus, value);

	return count;
}

static ssize_t raw16_store(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t count)
{
	ssize_t ret;

	if (!mutex_trylock(&gbx_profile_lock))
		return -EBUSY;
	if ((gbx_md_test || (gbx_profile_bus && gbx_profile_bus->gbx_active)))
		ret = -EBUSY;
	else
		ret = raw16_store_raw(dev, attr, buf, count);
	mutex_unlock(&gbx_profile_lock);
	return ret;
}

static DEVICE_ATTR_RW(raw16);

static ssize_t control_show_raw(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	struct retrode3_slot *slot = dev_get_drvdata(dev);

	buf[0] = 0;

	sprintf(buf+strlen(buf), "cd=%d\n", gpiod_get_value_cansleep(slot->cd));
	sprintf(buf+strlen(buf), "ce=%d\n", gpiod_get_value_cansleep(slot->ce));
	sprintf(buf+strlen(buf), "phi2=%d\n", gpiod_get_value_cansleep(slot->bus->phi2));
	sprintf(buf+strlen(buf), "rd=%d\n", gpiod_get_value_cansleep(slot->bus->oe));
	sprintf(buf+strlen(buf), "reset=%d\n", gpiod_get_value_cansleep(slot->bus->reset));
	sprintf(buf+strlen(buf), "we0=%d\n", gpiod_get_value_cansleep(slot->bus->we->desc[0]));
	sprintf(buf+strlen(buf), "we8=%d\n", gpiod_get_value_cansleep(slot->bus->we->desc[1]));
	sprintf(buf+strlen(buf), "time=%d\n", gpiod_get_value_cansleep(slot->bus->time));

	return strlen(buf);
}

static ssize_t control_show(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	ssize_t ret;

	if (!mutex_trylock(&gbx_profile_lock))
		return -EBUSY;
	if ((gbx_md_test || (gbx_profile_bus && gbx_profile_bus->gbx_active)))
		ret = -EBUSY;
	else
		ret = control_show_raw(dev, attr, buf);
	mutex_unlock(&gbx_profile_lock);
	return ret;
}


static ssize_t control_store_raw(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct retrode3_slot *slot = dev_get_drvdata(dev);
	unsigned int value;
	int err;

	if (!is_selected(slot))
		return -EACCES;

#define CONTROL(NAME, GPIO) if (strncmp(buf, #NAME "=", strlen(#NAME "=")) == 0) { \
		err = kstrtouint(buf+strlen(#NAME "="), 16, &value); \
		if (err < 0) \
			return err; \
		if (value > 1) \
			return -EINVAL; \
		gpiod_set_value_cansleep(GPIO, value); \
		return count; \
	}

	CONTROL(ce, slot->ce);
	CONTROL(phi2, slot->bus->phi2);
	CONTROL(rd, slot->bus->oe);
	CONTROL(reset, slot->bus->reset);
	CONTROL(we0, slot->bus->we->desc[0]);
	CONTROL(we8, slot->bus->we->desc[1]);
	CONTROL(time, slot->bus->time);

//printk("%s: %s", __func__, buf);
//#define TEST(NAME) printk("%s: %s len=%d arg=%s cmp=%d", __func__, #NAME "=", strlen(#NAME "="), buf+strlen(#NAME "="), strncmp(buf, #NAME "=", strlen(#NAME "=")));
//TEST(test);
	return -EINVAL;
}

static ssize_t control_store(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t count)
{
	ssize_t ret;

	if (!mutex_trylock(&gbx_profile_lock))
		return -EBUSY;
	if ((gbx_md_test || (gbx_profile_bus && gbx_profile_bus->gbx_active)))
		ret = -EBUSY;
	else
		ret = control_store_raw(dev, attr, buf, count);
	mutex_unlock(&gbx_profile_lock);
	return ret;
}

static DEVICE_ATTR_RW(control);

static int get_channel_index(struct device_attribute *attr)
{ // name ends in channel number
	return attr->attr.name[strlen(attr->attr.name) - 1] - '0';
}

static ssize_t clk_frequency_show_raw(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct retrode3_slot *slot = dev_get_drvdata(dev);
	int ch = get_channel_index(attr);

	return sysfs_emit(buf, "%lu\n", get_frequency(slot->bus, ch));
}

static ssize_t clk_frequency_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	ssize_t ret;

	if (!mutex_trylock(&gbx_profile_lock))
		return -EBUSY;
	if ((gbx_md_test || (gbx_profile_bus && gbx_profile_bus->gbx_active)))
		ret = -EBUSY;
	else
		ret = clk_frequency_show_raw(dev, attr, buf);
	mutex_unlock(&gbx_profile_lock);
	return ret;
}


static ssize_t clk_frequency_store_raw(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct retrode3_slot *slot = dev_get_drvdata(dev);
	int ch = get_channel_index(attr);
	unsigned long target_rate;
	int ret;

	ret = kstrtoul(buf, 10, &target_rate);
	if (ret)
		return ret;

	ret = set_frequency(slot->bus, ch, target_rate);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t clk_frequency_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	ssize_t ret;

	if (!mutex_trylock(&gbx_profile_lock))
		return -EBUSY;
	if ((gbx_md_test || (gbx_profile_bus && gbx_profile_bus->gbx_active)))
		ret = -EBUSY;
	else
		ret = clk_frequency_store_raw(dev, attr, buf, count);
	mutex_unlock(&gbx_profile_lock);
	return ret;
}


static DEVICE_ATTR(clock0, 0644, clk_frequency_show, clk_frequency_store);
static DEVICE_ATTR(clock1, 0644, clk_frequency_show, clk_frequency_store);
static DEVICE_ATTR(clock2, 0644, clk_frequency_show, clk_frequency_store);

static struct attribute *retrode3_attrs[] = {
	&dev_attr_sense.attr,
	&dev_attr_vcc.attr,
	&dev_attr_addr.attr,
	&dev_attr_data8.attr,
	&dev_attr_data16.attr,
	&dev_attr_raw16.attr,
	&dev_attr_control.attr,
	&dev_attr_clock0.attr,
	&dev_attr_clock1.attr,
	&dev_attr_clock2.attr,
	NULL,
};

static umode_t retrode3_attr_visibility(struct kobject *kobj, struct attribute *attr, int index) {

	// nur für SNES-Slot!
	// sollte anders gelöst werden (device-Tree sollte beschreiben wer die Attribute hat)

	if (attr == &dev_attr_clock2.attr) {
		if (!true) {
			return 0; // 0 bedeutet: Unsichtbar (wird nicht erstellt)
		}
	}

	return attr->mode; // Standard-Zugriffsrechte (z.B. 0444) zurückgeben
}

static const struct attribute_group retrode3_attr_group = {
	.attrs = retrode3_attrs,
	.is_visible = retrode3_attr_visibility,
};

static const struct attribute_group *retrode3_groups[] = {
	&retrode3_attr_group,
	NULL,
};

static int __init retrode3_module_init(void)
{
	int ret;

	ret = alloc_chrdev_region(&retrode3_first, 0, RETRODE3_MINORS, "retrode3");
	if (ret < 0) {
		pr_err("failed to allocate device numbers: %d\n", ret);
		return ret;
	}

	retrode3_class = class_create("retrode3");
	if (IS_ERR(retrode3_class)) {
		ret = PTR_ERR(retrode3_class);
		pr_err("failed to create class: %d\n", ret);
		goto err_unregister_chrdev;
	}

	retrode3_class->dev_groups = retrode3_groups;	// for additional /sys attributes

	ret = platform_driver_register(&retrode3_driver);

	if (ret < 0)
		goto err_destroy_class;

	pr_info("retrode3 driver registered with major %d\n", MAJOR(retrode3_first));

	return ret;

err_destroy_class:
	class_destroy(retrode3_class);
err_unregister_chrdev:
	unregister_chrdev_region(retrode3_first, RETRODE3_MINORS);

	return ret;
}

static void __exit retrode3_module_exit(void)
{
	/* The platform .remove callback still needs the class and the IDA. */
	platform_driver_unregister(&retrode3_driver);
	class_destroy(retrode3_class);
	unregister_chrdev_region(retrode3_first, RETRODE3_MINORS);
	ida_destroy(&retrode3_minors);
}

module_init(retrode3_module_init);
module_exit(retrode3_module_exit);

MODULE_ALIAS("retrode3");

MODULE_AUTHOR("H. Nikolaus Schaller <hns@goldelico.com>");
MODULE_DESCRIPTION("retrode 3 bus driver");
MODULE_LICENSE("GPL v2");

// SPDX-License-Identifier: GPL-2.0
/*
 * Retrode3 Mega Drive CFI bench transport (query only).
 *
 * No generic raw write, AMD unlock sequence, erase, program or SRAM access.
 * This file is included by retrode3_cart.c inside the existing module.
 *
 * WARNING: 0x98 itself is a cartridge WRITE cycle. This opt-in diagnostic
 * must never be called by ROM auto-detection or on unknown 5-V adapters.
 */
#include <linux/capability.h>
#include <linux/ioctl.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include "retrode3_flash_ioctl.h"

static int retrode3_flash_release_data(struct retrode3_bus *bus)
{
	int i, err, first = 0;

	for (i = 0; i < 16; i++) {
		err = gpiod_direction_input(bus->datas->desc[i]);
		if (err && !first)
			first = err;
	}
	return first;
}

/*
 * A single 16-bit Flash command cycle. Address and data settle with
 * /CE inactive; /OE remains inactive throughout. Both byte write lanes
 * must pulse together for a full-word CFI command.
 *
 * All error paths release /WE, /CE and the data drivers. This is the only
 * permitted bus write primitive in the diagnostic, and its two call sites
 * are restricted to CFI entry and reset below.
 */
static int retrode3_flash_word_command(struct retrode3_slot *slot,
				       u32 byte_address, u16 data)
{
	struct retrode3_bus *bus = slot->bus;
	int i, err = 0, cleanup;

	gpiod_set_value(bus->oe, 1);
	gpiod_set_value(bus->we->desc[0], 0);
	gpiod_set_value(bus->we->desc[1], 0);
	gpiod_set_value(slot->ce, 0);
	err = set_address(bus, byte_address);
	if (err)
		goto out;
	for (i = 0; i < 16; i++) {
		err = gpiod_direction_output(bus->datas->desc[i],
					    !!(data & BIT(i)));
		if (err)
			goto out;
	}
	udelay(1);
	gpiod_set_value(slot->ce, 1);
	udelay(1);
	gpiod_set_value(bus->we->desc[0], 1);
	gpiod_set_value(bus->we->desc[1], 1);
	udelay(1);
	gpiod_set_value(bus->we->desc[0], 0);
	gpiod_set_value(bus->we->desc[1], 0);
	udelay(1);
out:
	gpiod_set_value(bus->we->desc[0], 0);
	gpiod_set_value(bus->we->desc[1], 0);
	gpiod_set_value(slot->ce, 0);
	cleanup = retrode3_flash_release_data(bus);
	if (!err)
		err = cleanup;
	return err;
}

/*
 * Only a known native 16-bit MD socket, in 3.3-V mode, with a physically
 * present cartridge may enter the bench path. Do not turn this into an
 * automatic adapter-recognition mechanism. A second occupied cartridge,
 * FRAM mode, or either of the incompatible legacy/GBx experiments blocks it.
 */
static int retrode3_flash_check_slot(struct retrode3_slot *slot)
{
	struct retrode3_bus *bus = slot->bus;
	int i, present, voltage_direction;

	if (!of_device_is_compatible(slot->dev.of_node,
			"openpandora,retrode3-slot-megadrive") ||
	    slot->bus_width != 16 || slot->addr_width < 8 ||
	    IS_ERR_OR_NULL(bus->oe) || IS_ERR_OR_NULL(bus->time) ||
	    IS_ERR_OR_NULL(bus->datas) || bus->datas->ndescs != 16 ||
	    IS_ERR_OR_NULL(bus->we) || bus->we->ndescs < 2 ||
	    IS_ERR_OR_NULL(slot->ce) || IS_ERR_OR_NULL(slot->cd) ||
	    IS_ERR_OR_NULL(slot->power))
		return -EOPNOTSUPP;
	if (gbx_md_test || bus->gbx_active || legacy_bridge_hiz_test ||
	    sms_mapper_write_test || slot->fram_mode)
		return -EBUSY;

	/* 3.3 V is selected by a floating power-control GPIO. A negative
	 * gpiod_get_direction() result must NOT accidentally count as 3.3 V.
	 * Never change the voltage inside a query operation. */
	voltage_direction = gpiod_get_direction(slot->power);
	if (voltage_direction < 0)
		return voltage_direction;
	if (voltage_direction != 1)
		return -EPERM;

	present = gpiod_get_value(slot->cd);
	if (present <= 0)
		return present < 0 ? present : -ENODEV;
	for (i = 0; i < ARRAY_SIZE(bus->slots); i++) {
		struct retrode3_slot *other = bus->slots[i];

		if (!other || other == slot || other->is_controller ||
		    IS_ERR_OR_NULL(other->cd))
			continue;
		present = gpiod_get_value(other->cd);
		if (present < 0)
			return present;
		if (present)
			return -EBUSY;
	}
	return 0;
}

/*
 * CFI word address 0x55 -> MD byte address 0xAA. CFI query table word
 * address N -> byte address 2*N. Require 00xx query words (x16 mode).
 * Always send 0xF0 and release the bus even when QRY is missing.
 */
static int retrode3_flash_cfi_query(struct retrode3_slot *slot,
				   struct retrode3_flash_cfi_query_v1 *reply)
{
	struct retrode3_bus *bus = slot->bus;
	int err, cleanup, reset_err = 0;
	unsigned int i, last = 0x2d;
	bool command_attempted = false;

	if (!mutex_trylock(&bus->select_lock))
		return -EBUSY;

	err = retrode3_flash_check_slot(slot);
	if (err)
		goto unlock;
	/* Keep all chip selects inactive until address/data are safe. */
	gpiod_set_value(bus->oe, 1);
	gpiod_set_value(bus->we->desc[0], 0);
	gpiod_set_value(bus->we->desc[1], 0);
	gpiod_set_value(bus->time, 0);
	for (i = 0; i < ARRAY_SIZE(bus->slots); i++) {
		struct retrode3_slot *other = bus->slots[i];

		if (other && !other->is_controller && !IS_ERR_OR_NULL(other->ce))
			gpiod_set_value(other->ce, 0);
	}
	err = retrode3_flash_release_data(bus);
	if (err)
		goto restore;

	command_attempted = true;
	err = retrode3_flash_word_command(slot, 0x55 * 2, 0x0098);
	if (err)
		goto restore;
	udelay(1);
	gpiod_set_value(slot->ce, 1);
	for (i = 0; i < last; i++) {
		int value;

		err = set_address(bus, i * 2);
		if (err)
			goto restore;
		value = read_word(bus);
		if (value < 0) {
			err = value;
			goto restore;
		}
		if (value & 0xff00) {
			err = -ENODEV;
			goto restore;
		}
		reply->data[i] = (u8)value;
		if (i == 0x12 &&
		    (reply->data[0x10] != 'Q' ||
		     reply->data[0x11] != 'R' ||
		     reply->data[0x12] != 'Y')) {
			err = -ENODEV;
			goto restore;
		}
		/* Query only the standard table through the last erase-region
		 * descriptor. Undefined high query addresses may legitimately
		 * return floating data; don't reject a valid flash for that. */
		if (i == 0x2c) {
			if (!reply->data[0x2c] || reply->data[0x2c] > 8) {
				err = -ENODEV;
				goto restore;
			}
			last = 0x2d + 4 * reply->data[0x2c];
			if (last > RETRODE3_FLASH_CFI_QUERY_BYTES) {
				err = -ERANGE;
				goto restore;
			}
		}
	}
	reply->bus_width_bits = 16;
	reply->query_bytes = last;
	err = 0;
restore:
	/* A reset is attempted after EVERY command attempt, including a
	 * failed/partial CFI entry or failed read. Neither command depends
	 * on the CFI response, and reset is always bounded to 0xF0. */
	gpiod_set_value(bus->oe, 1);
	gpiod_set_value(bus->we->desc[0], 0);
	gpiod_set_value(bus->we->desc[1], 0);
	gpiod_set_value(slot->ce, 0);
	if (command_attempted)
		reset_err = retrode3_flash_word_command(slot, 0, 0x00f0);
	gpiod_set_value(bus->oe, 1);
	gpiod_set_value(bus->we->desc[0], 0);
	gpiod_set_value(bus->we->desc[1], 0);
	gpiod_set_value(bus->time, 0);
	gpiod_set_value(slot->ce, 0);
	cleanup = retrode3_flash_release_data(bus);
	if (!err && reset_err)
		err = reset_err;
	if (!err && cleanup)
		err = cleanup;
	if (reset_err || cleanup)
		dev_err(&slot->dev,
			"CFI bench bus cleanup failed: reset=%d input=%d\n",
			reset_err, cleanup);
unlock:
	mutex_unlock(&bus->select_lock);
	return err;
}

static long retrode3_flash_ioctl(struct file *file, unsigned int cmd,
				 unsigned long argument)
{
	struct retrode3_slot *slot = file->private_data;
	struct retrode3_flash_cfi_query_v1 data;
	void __user *arg = (void __user *)argument;
	int ret;

	if (cmd != RETRODE3_FLASH_IOC_CFI_QUERY)
		return -ENOTTY;
	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;
	if (!READ_ONCE(flash_cfi_test))
		return -EOPNOTSUPP;
	if (copy_from_user(&data, arg, sizeof(data)))
		return -EFAULT;
	if (data.abi_version != RETRODE3_FLASH_CFI_ABI_V1 || data.flags)
		return -EINVAL;
	memset(&data, 0, sizeof(data));
	data.abi_version = RETRODE3_FLASH_CFI_ABI_V1;
	ret = retrode3_flash_cfi_query(slot, &data);
	if (ret)
		return ret;
	if (copy_to_user(arg, &data, sizeof(data)))
		return -EFAULT;
	return 0;
}

// SPDX-License-Identifier: GPL-2.0
/*
 * Experimental x16 AMD-command-set Flash support for the tested
 * DragonBox 4MiB MegaDrive cartridge (EN29LV320CB, bottom boot).
 * Not registered for auto-detection and NOT a generic raw bus writer.
 * Included by retrode3_flash.c, after the CFI primitives.
 */
#include <linux/jiffies.h>
#include <linux/sched.h>
#include <linux/errno.h>

#define R3_FLASH_SIZE           4194304U
#define R3_FLASH_BOOT_SIZE      65536U
#define R3_FLASH_SMALL_SECTOR    8192U
#define R3_FLASH_LARGE_SECTOR   65536U

static int r3_flash_array_word(struct retrode3_slot *slot,
                               u32 addr, u16 *out)
{
    struct retrode3_bus *bus = slot->bus;
    int value, err;

    if (addr >= R3_FLASH_SIZE || (addr & 1))
        return -EINVAL;
    gpiod_set_value(bus->oe, 0); /* logical inactive: physical /OE high */
    gpiod_set_value(bus->we->desc[0], 0);
    gpiod_set_value(bus->we->desc[1], 0);
    gpiod_set_value(slot->ce, 0);
    err = (set_address)(bus, addr);
    if (err)
        return err;
    gpiod_set_value(slot->ce, 1);
    value = read_word(bus); /* toggles physical /OE during sampling */
    gpiod_set_value(slot->ce, 0);
    gpiod_set_value(bus->oe, 0);
    if (value < 0)
        return value;
    *out = (u16)value;
    return 0;
}

static int r3_flash_reset(struct retrode3_slot *slot)
{
    return retrode3_flash_word_command(slot, 0, 0x00f0);
}

/* AMD unlock addresses in the 16-bit word mode, represented as MD
 * byte addresses. These exactly match the provided flashkit.c. */
static int r3_flash_unlock(struct retrode3_slot *slot)
{
    int err = retrode3_flash_word_command(slot, 0xaaa, 0x00aa);
    if (err)
        return err;
    return retrode3_flash_word_command(slot, 0x554, 0x0055);
}

/* All operations first establish actual CFI geometry and the chip IDs.
 * No arbitrary address, voltage, ID or command is supplied from userspace.
 * CFI query and autoselect always restore normal array read mode. */
static int r3_flash_identify(struct retrode3_slot *slot,
                             struct retrode3_flash_id_v1 *id)
{
    struct retrode3_flash_cfi_query_v1 cfi = {
        .abi_version = RETRODE3_FLASH_CFI_ABI_V1,
    };
    struct retrode3_bus *bus = slot->bus;
    u32 total = 0;
    int err, reset_err, i;
    u16 manufacturer = 0, device = 0;

    err = retrode3_flash_cfi_query(slot, &cfi);
    if (err)
        return err;
    if (cfi.query_bytes < 0x35 || cfi.data[0x10] != 'Q' ||
        cfi.data[0x11] != 'R' || cfi.data[0x12] != 'Y' ||
        cfi.data[0x13] != 2 || cfi.data[0x14] != 0 ||
        cfi.data[0x27] != 22 || cfi.data[0x2c] != 2 ||
        cfi.data[0x28] != 2 || cfi.data[0x29] != 0)
        return -ENODEV;
    for (i = 0; i < 2; i++) {
        u32 pos = 0x2d + 4 * i;
        u32 blocks = (u32)cfi.data[pos] |
                     ((u32)cfi.data[pos + 1] << 8);
        u32 size_units = (u32)cfi.data[pos + 2] |
                         ((u32)cfi.data[pos + 3] << 8);
        u32 block_bytes = size_units << 8;
        blocks++;
        if ((i == 0 && (blocks != 8 || block_bytes != 8192)) ||
            (i == 1 && (blocks != 63 || block_bytes != 65536)))
            return -ENODEV;
        total += block_bytes * blocks;
    }
    if (total != R3_FLASH_SIZE)
        return -ENODEV;

    if (!mutex_trylock(&bus->select_lock))
        return -EBUSY;
    err = retrode3_flash_check_slot(slot);
    if (err)
        goto unlock;
    err = r3_flash_unlock(slot);
    if (err)
        goto restore;
    err = retrode3_flash_word_command(slot, 0xaaa, 0x0090);
    if (err)
        goto restore;
    /* Command-autoselect mode: manufacturer word 0, device word 1. */
    err = r3_flash_array_word(slot, 0, &manufacturer);
    if (err)
        goto restore;
    err = r3_flash_array_word(slot, 2, &device);
restore:
    reset_err = r3_flash_reset(slot);
    gpiod_set_value(bus->oe, 0);
    gpiod_set_value(bus->we->desc[0], 0);
    gpiod_set_value(bus->we->desc[1], 0);
    gpiod_set_value(slot->ce, 0);
    retrode3_flash_release_data(bus);
    if (!err)
        err = reset_err;
    if (!err) {
        /* Eon A8 low returns 0x7f; some variants return 0x1c. */
        if ((manufacturer & 0xff) != 0x7f &&
            (manufacturer & 0xff) != 0x1c)
            err = -ENODEV;
        if (device != 0x22f9)
            err = -ENODEV; /* bottom boot, not top boot */
    }
    if (!err) {
        id->manufacturer_id = manufacturer;
        id->device_id = device;
        id->capacity_bytes = R3_FLASH_SIZE;
        id->command_set = 2;
        id->sector_count = 71;
    }
unlock:
    mutex_unlock(&bus->select_lock);
    return err;
}

/* Pending state is sampled until the exact expected array word becomes
 * readable; never trust a fixed delay. The timeout bounds driver blocking.
 * Busy erase cannot always be cancelled: wait for the completion window. */
static int r3_flash_wait_word(struct retrode3_slot *slot, u32 address,
                              u16 expected, unsigned timeout_ms)
{
    unsigned long until = jiffies + msecs_to_jiffies(timeout_ms);
    u16 value;
    int err;

    do {
        err = r3_flash_array_word(slot, address, &value);
        if (err)
            return err;
        if (value == expected)
            return 0;
        if (time_after_eq(jiffies, until))
            break;
        usleep_range(40, 80);
        cond_resched();
    } while (1);
    return -ETIMEDOUT;
}

static int r3_flash_validate_sector(u32 address, u32 *bytes)
{
    if (address >= R3_FLASH_SIZE)
        return -ERANGE;
    if (address < R3_FLASH_BOOT_SIZE) {
        if (address % R3_FLASH_SMALL_SECTOR)
            return -EINVAL;
        *bytes = R3_FLASH_SMALL_SECTOR;
    } else {
        if ((address - R3_FLASH_BOOT_SIZE) % R3_FLASH_LARGE_SECTOR)
            return -EINVAL;
        *bytes = R3_FLASH_LARGE_SECTOR;
    }
    return 0;
}

static int r3_flash_erase_sector(struct retrode3_slot *slot,
                                 struct retrode3_flash_erase_v1 *request)
{
    struct retrode3_flash_id_v1 id = { 0 };
    struct retrode3_bus *bus = slot->bus;
    u32 length, offset;
    u16 word;
    int err, reset_err;

    err = r3_flash_validate_sector(request->offset_bytes, &length);
    if (err)
        return err;
    err = r3_flash_identify(slot, &id);
    if (err)
        return err;
    if (!mutex_trylock(&bus->select_lock))
        return -EBUSY;
    err = retrode3_flash_check_slot(slot);
    if (err)
        goto out;
    err = r3_flash_unlock(slot);
    if (err)
        goto reset;
    err = retrode3_flash_word_command(slot, 0xaaa, 0x0080);
    if (err)
        goto reset;
    err = r3_flash_unlock(slot);
    if (err)
        goto reset;
    err = retrode3_flash_word_command(slot, request->offset_bytes, 0x0030);
    if (err)
        goto reset;

    /* Sector erase can be much slower than program. Poll the last word
     * and then independently check EVERY word is really erased. */
    err = r3_flash_wait_word(slot, request->offset_bytes + length - 2,
                             0xffff, 60000);
    if (err)
        goto reset;
    for (offset = request->offset_bytes;
         offset < request->offset_bytes + length; offset += 2) {
        err = r3_flash_array_word(slot, offset, &word);
        if (err)
            goto reset;
        if (word != 0xffff) {
            err = -EIO;
            goto reset;
        }
        if ((offset & 0x1ff) == 0)
            cond_resched();
    }
reset:
    reset_err = r3_flash_reset(slot);
    if (!err)
        err = reset_err;
    gpiod_set_value(bus->oe, 0);
    gpiod_set_value(bus->we->desc[0], 0);
    gpiod_set_value(bus->we->desc[1], 0);
    gpiod_set_value(slot->ce, 0);
    retrode3_flash_release_data(bus);
out:
    mutex_unlock(&bus->select_lock);
    return err;
}

/* The tiny kernel-side chunk keeps each ioctl bounded. Write each physical
 * word in x16 mode; no raw address/command write is exposed to userspace. */
static int r3_flash_program(struct retrode3_slot *slot,
                             struct retrode3_flash_program_v1 *request)
{
    struct retrode3_flash_id_v1 id = { 0 };
    struct retrode3_bus *bus = slot->bus;
    u32 i;
    u16 old, target;
    int err, reset_err;

    if (!request->length || request->length > RETRODE3_FLASH_CHUNK_BYTES ||
        (request->length & 1) || (request->offset_bytes & 1) ||
        request->offset_bytes > R3_FLASH_SIZE - request->length)
        return -EINVAL;
    err = r3_flash_identify(slot, &id);
    if (err)
        return err;
    if (!mutex_trylock(&bus->select_lock))
        return -EBUSY;
    err = retrode3_flash_check_slot(slot);
    if (err)
        goto out;

    for (i = 0; i < request->length; i += 2) {
        u32 addr = request->offset_bytes + i;
        target = ((u16)request->data[i] << 8) |
                 request->data[i + 1]; /* MegaDrive file byte order */
        err = r3_flash_array_word(slot, addr, &old);
        if (err)
            break;
        if ((old & target) != target) {
            err = -EUCLEAN; /* needs erase to set a bit back to 1 */
            break;
        }
        if (old == target)
            continue;
        err = r3_flash_unlock(slot);
        if (err)
            break;
        err = retrode3_flash_word_command(slot, 0xaaa, 0x00a0);
        if (err)
            break;
        err = retrode3_flash_word_command(slot, addr, target);
        if (err)
            break;
        err = r3_flash_wait_word(slot, addr, target, 1000);
        if (err)
            break;
        cond_resched();
    }
    reset_err = r3_flash_reset(slot);
    if (!err)
        err = reset_err;
    gpiod_set_value(bus->oe, 0);
    gpiod_set_value(bus->we->desc[0], 0);
    gpiod_set_value(bus->we->desc[1], 0);
    gpiod_set_value(slot->ce, 0);
    retrode3_flash_release_data(bus);
out:
    mutex_unlock(&bus->select_lock);
    return err;
}

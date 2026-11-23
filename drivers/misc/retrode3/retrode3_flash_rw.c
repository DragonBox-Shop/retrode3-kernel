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

/*
 * Read TWO independent DQ6/DQ5 samples at the same status address.
 * The EN29LV320C allows successive reads with /CE asserted and /OE
 * pulsed for each read cycle. Set the address and assert /CE just ONCE
 * per pair, but keep a distinct physical /OE edge and the proven 1 us
 * settling delay for each sample. This is NOT a single read twice:
 * toggling requires two distinct /OE read cycles.
 *
 * All 16 Retrode data GPIOs MUST be inputs before either /OE pulse.
 * The preceding Flash word writer releases all 16 data drivers.
 * Keep both /WE lines inactive and restore /OE and /CE on every exit.
 * Only DQ6 (toggle) and DQ5 (timeout) are used by the status algorithm.
 */
static int r3_flash_status_pair(struct retrode3_slot *slot,
                                u32 addr, u16 *first, u16 *second)
{
    struct retrode3_bus *bus = slot->bus;
    u16 *samples[2] = { first, second };
    int err, dq6, dq5, i;

    if (addr >= R3_FLASH_SIZE || (addr & 1))
        return -EINVAL;

    /* Data is input; physical /OE high, both /WE inactive, /CE off. */
    gpiod_set_value(bus->oe, 0);
    gpiod_set_value(bus->we->desc[0], 0);
    gpiod_set_value(bus->we->desc[1], 0);
    gpiod_set_value(slot->ce, 0);
    err = (set_address)(bus, addr);
    if (err)
        goto restore;

    gpiod_set_value(slot->ce, 1);
    for (i = 0; i < 2; ++i) {
        /* set_bus_bit() uses physical levels; /OE low starts the
         * next read cycle, high ends it. /CE remains selected. */
        set_bus_bit(bus->oe, 0);
        udelay(1);
        dq6 = get_bus_bit(bus->datas->desc[6]);
        dq5 = get_bus_bit(bus->datas->desc[5]);
        set_bus_bit(bus->oe, 1);
        if (dq6 < 0) {
            err = dq6;
            break;
        }
        if (dq5 < 0) {
            err = dq5;
            break;
        }
        *samples[i] = (dq6 ? BIT(6) : 0) | (dq5 ? BIT(5) : 0);
    }

restore:
    gpiod_set_value(bus->oe, 0);
    gpiod_set_value(slot->ce, 0);
    return err;
}

/*
 * Two unlock-bypass program bus cycles: A0 at 0xAAA, then one ROM word.
 * While /CE, /WE and /OE are inactive BETWEEN the two cycles, the
 * Retrode can keep driving the same 16 GPIO outputs safely. Only changed
 * data bits need updating for the second command. Release ALL 16 pins
 * before the caller checks DQ5/DQ6, because the Flash will then drive
 * those lines through the cartridge level translators.
 *
 * Do not use for CFI, erase, SRAM, legacy adapters or other cartridges.
 */
static int r3_flash_bypass_program_word(struct retrode3_slot *slot,
                                         u32 addr, u16 target)
{
    struct retrode3_bus *bus = slot->bus;
    int i, err, cleanup;
    const u16 setup = 0x00a0;

    if (addr >= R3_FLASH_SIZE || (addr & 1))
        return -EINVAL;

    gpiod_set_value(bus->oe, 0); /* Flash outputs disabled */
    gpiod_set_value(bus->we->desc[0], 0);
    gpiod_set_value(bus->we->desc[1], 0);
    gpiod_set_value(slot->ce, 0);

    err = (set_address)(bus, 0xaaa);
    if (err)
        goto release;
    for (i = 0; i < 16; i++) {
        err = gpiod_direction_output(bus->datas->desc[i],
                                     !!(setup & BIT(i)));
        if (err)
            goto release;
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

    /* /CE inactive and both /WE inactive before changing address/data. */
    gpiod_set_value(slot->ce, 0);
    err = (set_address)(bus, addr);
    if (err)
        goto release;
    for (i = 0; i < 16; i++) {
        if ((setup ^ target) & BIT(i))
            /* Data bits are active-high raw lines. This matches
             * get_bus_bit() and the existing direct address GPIO path;
             * /CE and /WE are inactive during these updates. */
            set_bus_bit(bus->datas->desc[i], !!(target & BIT(i)));
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

release:
    gpiod_set_value(bus->we->desc[0], 0);
    gpiod_set_value(bus->we->desc[1], 0);
    gpiod_set_value(slot->ce, 0);
    cleanup = retrode3_flash_release_data(bus);
    if (!err)
        err = cleanup;
    return err;
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
        cfi.data[0x28] != 2 || cfi.data[0x29] != 0) {
        dev_warn(&slot->dev,
                 "Flash ID: CFI table mismatch: len=%u cmd=%02x%02x size_exp=%u interface=%02x%02x regions=%u\n",
                 cfi.query_bytes, cfi.data[0x14], cfi.data[0x13],
                 cfi.data[0x27], cfi.data[0x29], cfi.data[0x28],
                 cfi.data[0x2c]);
        return -ENODEV;
    }
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

    /* A concurrent short kernel-side bus operation is not a cartridge
     * fault. Wait for the mutex instead of spuriously failing mid-job. */
    if (mutex_lock_interruptible(&bus->select_lock))
        return -ERESTARTSYS;
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
        if (((manufacturer & 0xff) != 0x7f &&
             (manufacturer & 0xff) != 0x1c) || device != 0x22f9) {
            dev_warn(&slot->dev,
                     "Flash ID: unsupported manufacturer=%04x device=%04x (expected EN29LV320CB 22f9)\n",
                     (unsigned int)manufacturer, (unsigned int)device);
            err = -ENODEV;
        }
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

/* The chip signals embedded erase/program completion by stopping DQ6
 * toggling. This is status polling, NOT full data verification.
 * If DQ5 reports a possible timeout, sample DQ6 twice again as required
 * by the EN29LV320C toggle-bit flowchart: DQ6 may have just stopped.
 * Small word-program operations are not delayed by 40-80us sleeps;
 * sector erase retains a short sleep to avoid busy-waiting. */
static int r3_flash_wait_ready(struct retrode3_slot *slot, u32 address,
                               unsigned timeout_ms)
{
    unsigned long until = jiffies + msecs_to_jiffies(timeout_ms);
    u16 previous = 0, sample = 0;
    int err;

    do {
        err = r3_flash_status_pair(slot, address, &previous, &sample);
        if (err)
            return err;
        if (!((previous ^ sample) & BIT(6)))
            return 0;
        if (sample & BIT(5)) {
            /* DQ5 timeout might race with command completion. */
            err = r3_flash_status_pair(slot, address, &previous, &sample);
            if (err)
                return err;
            if (!((previous ^ sample) & BIT(6)))
                return 0;
            dev_warn(&slot->dev,
                     "Flash poll: DQ5 timeout at 0x%08x: DQ6 still toggling (%04x/%04x)\n",
                     address, (unsigned int)previous,
                     (unsigned int)sample);
            return -EIO;
        }
        if (time_after_eq(jiffies, until))
            break;
        if (timeout_ms > 1000)
            usleep_range(40, 80);
        cond_resched();
    } while (1);
    dev_warn(&slot->dev,
             "Flash poll: timeout at 0x%08x: first=%04x next=%04x DQ5=%u\n",
             address, (unsigned int)previous,
             (unsigned int)sample, !!(sample & BIT(5)));
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

/* Only the FD that completed a matching CFI + ID check may write.
 * The GPIO slot check still runs before EVERY destructive ioctl; any
 * observed insertion/removal invalidates the authorization. */
static int r3_flash_check_session(struct retrode3_file_context *ctx)
{
    struct retrode3_slot *slot = ctx->slot;

    if (!ctx->flash_identified ||
        ctx->flash_cd_epoch != atomic_read(&slot->cd_epoch)) {
        ctx->flash_identified = false;
        return -ENODEV;
    }
    return retrode3_flash_check_slot(slot);
}

static int r3_flash_erase_sector(struct retrode3_file_context *ctx,
                                 struct retrode3_flash_erase_v1 *request)
{
    struct retrode3_slot *slot = ctx->slot;
    struct retrode3_bus *bus = slot->bus;
    u32 length;
    int err, reset_err;
    const char *stage = "validate-sector";

    err = r3_flash_validate_sector(request->offset_bytes, &length);
    if (err)
        return err;
    /* Identity was established at the start of this open FD's session.
     * No repeated CFI entry on each sector. */
    /* A concurrent short kernel-side bus operation is not a cartridge
     * fault. Wait for the mutex instead of spuriously failing mid-job. */
    if (mutex_lock_interruptible(&bus->select_lock))
        return -ERESTARTSYS;
    stage = "session-and-slot-check";
    err = r3_flash_check_session(ctx);
    if (err)
        goto out;
    stage = "unlock";
    err = r3_flash_unlock(slot);
    if (err)
        goto reset;
    stage = "erase-setup";
    err = retrode3_flash_word_command(slot, 0xaaa, 0x0080);
    if (err)
        goto reset;
    stage = "erase-unlock";
    err = r3_flash_unlock(slot);
    if (err)
        goto reset;
    stage = "erase-command";
    err = retrode3_flash_word_command(slot, request->offset_bytes, 0x0030);
    if (err)
        goto reset;

    /* The status-poll below establishes erase completion for this
     * sector. Do NOT read back every word: a full image verification,
     * including repeat/paranoid passes, belongs to the CLI Verify phase.
     * This avoids 32,768 slow GPIO reads after each 64 KiB erase. */
    stage = "poll";
    err = r3_flash_wait_ready(slot, request->offset_bytes + length - 2,
                              60000);
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
    if (err)
        dev_warn(&slot->dev,
                 "Flash erase: sector 0x%08x failed at %s (%d)\n",
                 request->offset_bytes, stage, err);
    mutex_unlock(&bus->select_lock);
    return err;
}

/* The tiny kernel-side chunk keeps each ioctl bounded. Write each physical
 * word in x16 mode; no raw address/command write is exposed to userspace. */
static int r3_flash_program(struct retrode3_file_context *ctx,
                             struct retrode3_flash_program_v1 *request)
{
    struct retrode3_slot *slot = ctx->slot;
    struct retrode3_bus *bus = slot->bus;
    u32 i;
    u16 target;
    int err, reset_err, bypass_err;
    bool bypass = false;

    if (!request->length || request->length > RETRODE3_FLASH_CHUNK_BYTES ||
        (request->length & 1) || (request->offset_bytes & 1) ||
        request->offset_bytes > R3_FLASH_SIZE - request->length)
        return -EINVAL;
    /* Already identified on this open FD. Never repeat CFI for each
     * 512-byte program chunk. An invalidated session must reprobe. */
    /* A concurrent short kernel-side bus operation is not a cartridge
     * fault. Wait for the mutex instead of spuriously failing mid-job. */
    if (mutex_lock_interruptible(&bus->select_lock))
        return -ERESTARTSYS;
    err = r3_flash_check_session(ctx);
    if (err)
        goto out;

    for (i = 0; i < request->length; i += 2) {
        u32 addr = request->offset_bytes + i;
        target = ((u16)request->data[i] << 8) |
                 request->data[i + 1]; /* MegaDrive file byte order */
        /* Program directly: no extra old-ROM read or per-word content
         * check. Write-only on a non-erased chip can still fail according
         * to Flash status; a requested full Verify checks ROM contents
         * only AFTER all programming is complete. Unlock bypass removes
         * two command cycles per word after entry. */
        if (!bypass) {
            err = r3_flash_unlock(slot);
            if (err)
                break;
            err = retrode3_flash_word_command(slot, 0xaaa, 0x0020);
            if (err)
                break;
            bypass = true;
        }
        /* One paired A0/data sequence, then DQ6/DQ5 completion polling.
         * The helper releases all 16 outputs before the Flash drives
         * status data. Never issue the next word while DQ6 toggles. */
        err = r3_flash_bypass_program_word(slot, addr, target);
        if (err)
            break;
        err = r3_flash_wait_ready(slot, addr, 1000);
        if (err)
            break;
        cond_resched();
    }
    /* The unlock-bypass escape is 90h, then 00h, rather than a
     * generic F0h reset. Always exit even after partial failure.
     * Follow with the regular array reset for the next ioctl/user. */
    if (bypass) {
        bypass_err = retrode3_flash_word_command(slot, 0, 0x0090);
        if (!bypass_err)
            bypass_err = retrode3_flash_word_command(slot, 0, 0x0000);
        if (!err)
            err = bypass_err;
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

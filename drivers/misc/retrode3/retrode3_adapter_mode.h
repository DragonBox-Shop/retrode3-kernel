/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _RETRODE3_ADAPTER_MODE_H
#define _RETRODE3_ADAPTER_MODE_H

/*
 * Opt-in, read-only 8-bit transport primitives for passive slot adapters.
 * Mode is stored in bits 31..24 of /dev/slot-* file offset.
 * This does not change voltage, timing, RD/WR wiring or bus width.
 */
#define RETRODE3_ADAPTER_READ_D0_D7   20U
#define RETRODE3_ADAPTER_READ_D8_D15 21U
/*
 * Dedicated, isolated SMS-0007 mapper-register transport.
 * NOT a generic cartridge write operation.
 */
#define RETRODE3_ADAPTER_SMS_MD_MAPPER_WRITE 22U

/* Dedicated Retrode2 GBx-0011 MD-slot ROM-only bus transactions. */
#define RETRODE3_GBX_MD_GB_READ8       23U
#define RETRODE3_GBX_MD_GBA_READ16_LO  24U
#define RETRODE3_GBX_MD_GBA_READ16_HI  25U
/* Debug-only: each 16-bit little-endian word has MD A7..A18 GPIO levels
 * sampled during a GB /RD cycle at ROM offset (byte_offset >> 1).
 * Kernel requires 3.3V for this mode. */
#define RETRODE3_GBX_MD_GB_PINSCAN     26U

static inline int retrode3_gbx_mode_is_read(unsigned int mode)
{
    return mode == RETRODE3_GBX_MD_GB_READ8 ||
           mode == RETRODE3_GBX_MD_GBA_READ16_LO ||
           mode == RETRODE3_GBX_MD_GBA_READ16_HI ||
           mode == RETRODE3_GBX_MD_GB_PINSCAN;
}

static inline int retrode3_adapter_mode_is_read(unsigned int mode)
{
    return mode == RETRODE3_ADAPTER_READ_D0_D7 ||
           mode == RETRODE3_ADAPTER_READ_D8_D15;
}

/* Reject NES and absent lanes even if the numeric mode is requested. */
static inline int retrode3_adapter_lane_allowed(unsigned int mode,
                                                unsigned int bus_width,
                                                int is_md, int is_snes)
{
    if (mode == RETRODE3_ADAPTER_READ_D0_D7)
        return (is_md && bus_width == 16U) ||
               (is_snes && bus_width == 8U);
    if (mode == RETRODE3_ADAPTER_READ_D8_D15)
        return is_md && bus_width == 16U;
    return 0;
}

#endif

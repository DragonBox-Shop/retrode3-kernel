/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _RETRODE3_FLASH_IOCTL_H
#define _RETRODE3_FLASH_IOCTL_H

/*
 * PRIVATE EXPERIMENTAL ABI (Letux 7.3 bench). Not a stable cartridge UAPI.
 * This exposes a 16-bit AMD-style CFI/ID query plus restricted, explicitly
 * opt-in sector erase and bounded program operations for one known flash
 * device on the native Mega Drive slot. Not a generic raw-write ABI.
 *
 * The caller must explicitly select the Flash operation and hold the
 * shared Retrode3 userspace bus lock. Device-node permissions govern access.
 * The driver enforces the native 16-bit MD slot, correct 5V selection,
 * supported chip identity, geometry and valid sector/chunk boundaries.
 */
#include <linux/ioctl.h>
#include <linux/types.h>

#define RETRODE3_FLASH_CFI_ABI_V1 1U
#define RETRODE3_FLASH_CFI_QUERY_BYTES 128U

struct retrode3_flash_cfi_query_v1 {
	__u32 abi_version;	/* input: 1 */
	__u32 flags;		/* input: 0; reserved */
	__u32 bus_width_bits;	/* output: 16 */
	__u32 query_bytes;	/* output: valid prefix, at most 128 */
	__u8 data[RETRODE3_FLASH_CFI_QUERY_BYTES];
};

/* Experimental requests use native kernel ioctl encoding (including MIPS).
 * Only a matching Eon EN29LV320CB bottom-boot geometry may pass the
 * in-kernel identity/CFI check for erase and program operations. */
#define RETRODE3_FLASH_CHUNK_BYTES 512U
struct retrode3_flash_id_v1 {
    __u32 abi_version;       /* input: 1 */
    __u32 flags;             /* input: zero */
    __u32 manufacturer_id;   /* output */
    __u32 device_id;         /* output */
    __u32 capacity_bytes;    /* output */
    __u32 command_set;       /* output */
    __u32 sector_count;      /* output */
};
struct retrode3_flash_erase_v1 {
    __u32 abi_version;       /* input: 1 */
    __u32 flags;             /* input: zero */
    __u32 offset_bytes;      /* input: sector start */
};
struct retrode3_flash_program_v1 {
    __u32 abi_version;       /* input: 1 */
    __u32 flags;             /* input: zero */
    __u32 offset_bytes;      /* input: 2-byte aligned start */
    __u32 length;            /* input: even, 2..512 bytes */
    __u8 data[RETRODE3_FLASH_CHUNK_BYTES]; /* Sega ROM big-endian byte order */
};
/* Fixed CFI command 0x98 / word 0x55, plus 0xF0 reset on exit. */
#define RETRODE3_FLASH_IOC_CFI_QUERY 	_IOWR('R', 0xf0, struct retrode3_flash_cfi_query_v1)

#define RETRODE3_FLASH_IOC_IDENTIFY  _IOWR('R', 0xf1, struct retrode3_flash_id_v1)
#define RETRODE3_FLASH_IOC_ERASE     _IOW('R', 0xf2, struct retrode3_flash_erase_v1)
#define RETRODE3_FLASH_IOC_PROGRAM   _IOW('R', 0xf3, struct retrode3_flash_program_v1)

#endif

/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _RETRODE3_FLASH_IOCTL_H
#define _RETRODE3_FLASH_IOCTL_H

/*
 * PRIVATE EXPERIMENTAL ABI (Letux 7.3 bench). Not a stable cartridge UAPI.
 * This reads a 16-bit, word-addressed AMD-style CFI query table on the
 * native Mega Drive slot. It does not provide any erase/program primitive.
 *
 * The caller must explicitly select Write Cart, verify the supply voltage,
 * hold the shared Retrode3 userspace bus lock and ensure nobody else is
 * accessing a cartridge. flash_cfi_test defaults to OFF; root must opt in.
 */
#include <linux/ioctl.h>
#include <linux/types.h>

#define RETRODE3_FLASH_CFI_ABI_V1 1U
#define RETRODE3_FLASH_CFI_QUERY_BYTES 128U

struct retrode3_flash_cfi_query_v1 {
	__u32 abi_version;	/* input: 1 */
	__u32 flags;		/* input: 0; reserved */
	__u32 bus_width_bits;	/* output: 16 */
	__u32 query_bytes;	/* output: 128 */
	__u8 data[RETRODE3_FLASH_CFI_QUERY_BYTES];
};

/* Only fixed CFI command 0x98 / word 0x55, plus 0xF0 reset on exit. */
#define RETRODE3_FLASH_IOC_CFI_QUERY 	_IOWR('R', 0xf0, struct retrode3_flash_cfi_query_v1)

#endif

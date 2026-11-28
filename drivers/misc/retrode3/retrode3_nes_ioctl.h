/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _RETRODE3_NES_IOCTL_H
#define _RETRODE3_NES_IOCTL_H

#include <linux/ioctl.h>

/*
 * Experimental native NES reset-detector stimulus (v1).
 * No argument, fixed M2-low hold, no power or data write.
 * Success means the GPIO operation completed, not that a mapper
 * changed banks. Caller must hold the shared userspace bus lock.
 * Encode on the target architecture (MIPS differs from x86).
 */
#define RETRODE3_NES_IOC_SOFT_RESET _IO('R', 0xf4)

#endif

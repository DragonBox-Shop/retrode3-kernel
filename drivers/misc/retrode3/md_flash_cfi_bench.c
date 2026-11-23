// SPDX-License-Identifier: GPL-2.0
/*
 * Manually invoked userspace CFI bench for /dev/slot-md.
 * No erase or program. ioctl uses the target MIPS architecture's _IOC bits,
 * not a hardcoded x86 _IOWR value. Run only after operator approval.
 */
#include "retrode3_flash_ioctl.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static unsigned short le16(const unsigned char *p)
{
	return (unsigned short)(p[0] | ((unsigned int)p[1] << 8));
}

int main(int argc, char **argv)
{
	struct retrode3_flash_cfi_query_v1 query = {
		.abi_version = RETRODE3_FLASH_CFI_ABI_V1,
	};
	unsigned long long total = 0, size;
	unsigned int i, nregions;
	const unsigned char *p;
	int fd;

	if (argc != 2) {
		fprintf(stderr, "Usage: %s /dev/slot-md\n"
			"BENCH ONLY: root + flash_cfi_test=1, MD connector 5V, "
			"one known flash cartridge, GBx off.\n", argv[0]);
		return 2;
	}
	fd = open(argv[1], O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0) {
		fprintf(stderr, "Cannot open slot: %s\n", strerror(errno));
		return 1;
	}
	if (ioctl(fd, RETRODE3_FLASH_IOC_CFI_QUERY, &query) < 0) {
		fprintf(stderr, "CFI ioctl refused/failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	close(fd);
	p = query.data;
	if (query.abi_version != RETRODE3_FLASH_CFI_ABI_V1 ||
	    query.query_bytes < 0x2d ||
	    query.query_bytes > RETRODE3_FLASH_CFI_QUERY_BYTES ||
	    query.bus_width_bits != 16 ||
	    p[0x10] != 'Q' || p[0x11] != 'R' || p[0x12] != 'Y') {
		fprintf(stderr, "Invalid CFI response/ABI\n");
		return 1;
	}
	if (p[0x27] < 12 || p[0x27] > 30) {
		fprintf(stderr, "CFI capacity exponent invalid\n");
		return 1;
	}
	size = 1ULL << p[0x27];
	nregions = p[0x2c];
	if (!nregions || nregions > 8 ||
	    0x2dU + 4U * nregions > query.query_bytes) {
		fprintf(stderr, "CFI region table invalid\n");
		return 1;
	}
	printf("CFI: %c%c%c | primary command set: 0x%04X"
	       " | interface: 0x%04X | capacity: %llu bytes\n",
	       p[0x10], p[0x11], p[0x12], le16(p + 0x13),
	       le16(p + 0x28), size);
	for (i = 0; i < nregions; i++) {
		unsigned int offset = 0x2d + i * 4;
		unsigned long blocks = (unsigned long)le16(p + offset) + 1;
		unsigned long block_bytes =
			(unsigned long)le16(p + offset + 2) * 256UL;

		if (!block_bytes || blocks > (size - total) / block_bytes) {
			fprintf(stderr, "Invalid CFI region %u\n", i);
			return 1;
		}
		printf("  region %u: %lu blocks x %lu bytes @ 0x%08llX\n",
		       i, blocks, block_bytes, total);
		total += (unsigned long long)blocks * block_bytes;
	}
	if (total != size) {
		fprintf(stderr, "CFI region total does not match chip size\n");
		return 1;
	}
	puts("Geometry consistent; flash erase/program NOT enabled.");
	return 0;
}

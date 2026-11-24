// SPDX-License-Identifier: GPL-2.0
/*
 * Operator-only destructive Flash test for the known DragonBox MD 4MiB
 * EN29LV320CB cartridge. Requires two root-gated kernel module parameters,
 * exclusive /run/lock/retrode3/bus.lock via flock(1), and 5V at MD connector.
 * Compiles with target GCC 4.9. This is not a public writer or plugin.
 */
#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include "retrode3_flash_ioctl.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define FLASH_BYTES 4194304U
#define CHUNK RETRODE3_FLASH_CHUNK_BYTES

static volatile sig_atomic_t stopping;

static void on_interrupt(int signo)
{
    (void)signo;
    stopping = 1;
}

static int err_sys(const char *what)
{
    fprintf(stderr, "%s: %s\n", what, strerror(errno));
    return 1;
}

static int err_message(const char *what)
{
    fprintf(stderr, "%s\n", what);
    return 1;
}

/* NDJSON-style progress, emitted after completed hardware operations.
 * The common shared CLI will own these events in the real plugin. */
static void progress(const char *phase, unsigned long done,
                     unsigned long total, int sectors)
{
    static char last_phase[32];
    static unsigned last_percent = 101;
    unsigned percent = total ? (unsigned)((done * 100u) / total) : 100u;

    if (percent == last_percent && strcmp(last_phase, phase) == 0 &&
        done != total)
        return;
    snprintf(last_phase, sizeof(last_phase), "%s", phase);
    last_percent = percent;
    if (sectors)
        printf("{\"event\":\"progress\",\"phase\":\"%s\","
               "\"units\":\"sectors\",\"units_done\":%lu,"
               "\"units_total\":%lu,\"percent\":%u}\n",
               phase, done, total, percent);
    else
        printf("{\"event\":\"progress\",\"phase\":\"%s\","
               "\"units\":\"bytes\",\"bytes_done\":%lu,"
               "\"bytes_total\":%lu,\"percent\":%u}\n",
               phase, done, total, percent);
    fflush(stdout);
}

static int identify(int slotfd, struct retrode3_flash_id_v1 *id)
{
    memset(id, 0, sizeof(*id));
    id->abi_version = RETRODE3_FLASH_CFI_ABI_V1;
    if (ioctl(slotfd, RETRODE3_FLASH_IOC_IDENTIFY, id) < 0)
        return err_sys("Flash identify ioctl failed");
    if (id->capacity_bytes != FLASH_BYTES || id->sector_count != 71 ||
        id->command_set != 2 || id->device_id != 0x22f9 ||
        (id->manufacturer_id != 0x007f && id->manufacturer_id != 0x001c))
        return err_message("Flash ID is not the supported EN29LV320CB bottom-boot device");
    printf("Flash identified: manufacturer=%04x device=%04x "
           "capacity=%" PRIu32 " sectors=%" PRIu32 "\n",
           id->manufacturer_id, id->device_id,
           id->capacity_bytes, id->sector_count);
    return 0;
}

static int source_open(const char *name, unsigned long *total)
{
    struct stat st;
    int fd = open(name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        err_sys("Cannot open source file");
        return -1;
    }
    if (fstat(fd, &st) != 0) {
        err_sys("Cannot stat ROM file");
        close(fd);
        return -1;
    }
    if (!S_ISREG(st.st_mode) || st.st_size < 512 ||
        st.st_size > FLASH_BYTES || (st.st_size & 1)) {
        err_message("ROM must be a regular, even-sized file (512..4194304 bytes)");
        close(fd);
        return -1;
    }
    *total = (unsigned long)st.st_size;
    return fd;
}

static unsigned long sector_size(unsigned long pos)
{
    return pos < 65536ul ? 8192ul : 65536ul;
}

static unsigned long erase_sector_count(unsigned long total)
{
    unsigned long pos = 0, count = 0;
    while (pos < total) {
        pos += sector_size(pos);
        count++;
    }
    return count;
}

static int do_erase(int slotfd, unsigned long total)
{
    struct retrode3_flash_erase_v1 request;
    unsigned long pos = 0, done = 0;
    const unsigned long count = erase_sector_count(total);
    memset(&request, 0, sizeof(request));
    request.abi_version = RETRODE3_FLASH_CFI_ABI_V1;
    progress("erase", 0, count, 1);
    while (pos < total) {
        if (stopping)
            return err_message("Interrupted. Cartridge may be partially erased.");
        request.offset_bytes = (unsigned)pos;
        if (ioctl(slotfd, RETRODE3_FLASH_IOC_ERASE, &request) < 0)
            return err_sys("Erase failed; cartridge may be partially erased");
        pos += sector_size(pos);
        progress("erase", ++done, count, 1);
    }
    return 0;
}

static int do_program(int slotfd, int srcfd, unsigned long total)
{
    struct retrode3_flash_program_v1 request;
    unsigned long pos = 0;
    ssize_t count;
    memset(&request, 0, sizeof(request));
    request.abi_version = RETRODE3_FLASH_CFI_ABI_V1;
    progress("program", 0, total, 0);
    while (pos < total) {
        if (stopping)
            return err_message("Interrupted. Cartridge may be partially programmed.");
        request.offset_bytes = (unsigned)pos;
        request.length = total - pos > CHUNK ? CHUNK : (unsigned)(total - pos);
        count = pread(srcfd, request.data, request.length, (off_t)pos);
        if (count != (ssize_t)request.length)
            return err_message("ROM file changed or became unreadable during programming");
        if (ioctl(slotfd, RETRODE3_FLASH_IOC_PROGRAM, &request) < 0)
            return err_sys("Program failed; cartridge may be partially programmed");
        pos += request.length;
        progress("program", pos, total, 0);
    }
    return 0;
}

static int do_verify(int slotfd, int srcfd, unsigned long total)
{
    unsigned char expected[4096], actual[4096];
    unsigned long pos = 0;
    progress("verify", 0, total, 0);
    while (pos < total) {
        size_t n = total - pos > sizeof(expected) ?
                   sizeof(expected) : (size_t)(total - pos);
        ssize_t r;
        unsigned long j;

        if (stopping)
            return err_message("Verification interrupted");
        r = pread(srcfd, expected, n, (off_t)pos);
        if (r != (ssize_t)n)
            return err_message("Source ROM unreadable during verification");
        /* The native MD char-device reader returns requested contiguous
         * blocks; reject a short read rather than silently accepting it. */
        r = pread(slotfd, actual, n, (off_t)pos);
        if (r != (ssize_t)n)
            return err_message("Short/failed cartridge readback");
        for (j = 0; j < n; j++) {
            if (expected[j] != actual[j]) {
                fprintf(stderr,
                    "VERIFY MISMATCH at 0x%08lx: expected %02x got %02x\n",
                    pos + j, expected[j], actual[j]);
                return 1;
            }
        }
        pos += n;
        progress("verify", pos, total, 0);
    }
    printf("VERIFY PASSED: %lu bytes compared byte-for-byte\n", total);
    return 0;
}

int main(int argc, char **argv)
{
    struct retrode3_flash_id_v1 id;
    int slotfd, srcfd = -1, err = 0;
    unsigned long total = 0;
    const char *action;

    if (argc < 3 || argc > 4) {
        fprintf(stderr,
          "Usage: %s /dev/slot-md identify\n"
          "       %s /dev/slot-md verify /absolute/ROM.md\n"
          "       %s /dev/slot-md write /absolute/ROM.md\n"
          "       %s /dev/slot-md erase /absolute/ROM.md\n"
          "MD slot at 5V; supported Flash cart; external flock.\n",
          argv[0], argv[0], argv[0], argv[0]);
        return 2;
    }
    action = argv[2];
    if (strcmp(action, "identify") == 0) {
        if (argc != 3)
            return err_message("identify takes no file");
    } else if (strcmp(action, "verify") == 0) {
        if (argc != 4)
            return err_message("verify requires exactly one ROM path");
    } else if (strcmp(action, "erase") == 0 ||
               strcmp(action, "write") == 0) {
        if (argc != 4)
            return err_message("erase/write require a ROM source path");
    } else {
        return err_message("Unknown Flash action");
    }
    signal(SIGINT, on_interrupt);
    signal(SIGTERM, on_interrupt);
    slotfd = open(argv[1], O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (slotfd < 0)
        return err_sys("Cannot open MD slot");
    err = identify(slotfd, &id);
    if (err)
        goto out;
    if (argc >= 4) {
        if (argv[3][0] != '/') {
            err = err_message("ROM path must be absolute");
            goto out;
        }
        srcfd = source_open(argv[3], &total);
        if (srcfd < 0) {
            err = 1;
            goto out;
        }
    }
    if (strcmp(action, "erase") == 0 || strcmp(action, "write") == 0) {
        fprintf(stderr, "DESTRUCTIVE FLASH TEST: erase %lu sectors, "
                "%lu ROM bytes; do not remove power/cartridge.\n",
                erase_sector_count(total), total);
        err = do_erase(slotfd, total);
        if (err)
            goto out;
    }
    if (strcmp(action, "write") == 0) {
        err = do_program(slotfd, srcfd, total);
        if (err)
            goto out;
        err = do_verify(slotfd, srcfd, total);
    } else if (strcmp(action, "verify") == 0) {
        err = do_verify(slotfd, srcfd, total);
    }
out:
    if (srcfd >= 0)
        close(srcfd);
    close(slotfd);
    return err;
}

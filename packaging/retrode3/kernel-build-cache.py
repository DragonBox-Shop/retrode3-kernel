#!/usr/bin/env python3
"""Identify a compatible kernel build independently of private driver changes."""
import hashlib
from pathlib import Path
import re
import subprocess
import sys

DRIVER = b"drivers/misc/retrode3/"
# Only sources of the private single-object driver; Kconfig, Makefile and
# any new/unknown files remain in the base key by default.
PRIVATE = {DRIVER + name for name in (
    b"retrode3.c", b"retrode3_cart.c", b"retrode3_controller.c",
    b"retrode3_bus.h", b"retrode3_adapter_mode.h", b"retrode3_flash.c",
    b"retrode3_flash_ioctl.h", b"retrode3_flash_rw.c", b"retrode3_gbx.c",
    b"retrode3_slot.h",
)}
# Bump this when the full kernel Kbuild flags or cache layout change.
SCHEMA = b"retrode3-kernel-abi-v1"


def keys(tree, image_id, isolate=True):
    if not re.fullmatch(r"sha256:[0-9a-f]{64}", image_id):
        raise ValueError("Invalid kernel builder image identity")
    base = hashlib.sha256(SCHEMA + b"\0" + image_id.encode() + b"\0")
    driver = hashlib.sha256(b"retrode3-driver-v1\0")
    for record in tree.split(b"\0"):
        if not record:
            continue
        _, path = record.split(b"\t", 1)
        if path in PRIVATE:
            driver.update(record + b"\0")
            if isolate:
                continue
        if path.startswith((b".github/", b"packaging/retrode3/")) or path == b"AGENTS.md":
            continue
        base.update(record + b"\0")
    return base.hexdigest(), driver.hexdigest()


def main():
    root = Path(sys.argv[1])
    image_id = sys.argv[2]
    config = (root / "arch/mips/configs/retrode3_full.config").read_text()
    # With signing, symbol trimming, or exported driver symbols, a change
    # to the driver is not assumed to be independent of the kernel build.
    isolate = all(line in config.splitlines() for line in (
        "CONFIG_RETRODE3=m", "CONFIG_MODVERSIONS=y",
        "# CONFIG_MODULE_SIG is not set",
        "# CONFIG_TRIM_UNUSED_KSYMS is not set",
        "# CONFIG_MODULE_COMPRESS is not set",
    ))
    for path in PRIVATE:
        f = root / path.decode()
        if f.is_file() and re.search(rb"\bEXPORT_\w*SYMBOL\w*\s*\(", f.read_bytes()):
            isolate = False
    tree = subprocess.check_output(["git", "-C", str(root), "ls-tree", "-r", "-z", "HEAD"])
    print(*keys(tree, image_id, isolate))


if __name__ == "__main__":
    main()

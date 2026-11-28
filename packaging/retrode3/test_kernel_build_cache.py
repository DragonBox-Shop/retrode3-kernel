#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("cache", Path(__file__).with_name("kernel-build-cache.py"))
cache = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cache)
IMAGE = "sha256:" + "a" * 64


def tree(driver=b"one", kernel=b"base", kconfig=b"config", packaging=b"one"):
    return b"\0".join((
        b"100644 blob " + kernel + b"\tarch/mips/kernel/setup.c",
        b"100644 blob " + kconfig + b"\tdrivers/misc/retrode3/Kconfig",
        b"100644 blob " + driver + b"\tdrivers/misc/retrode3/retrode3_cart.c",
        b"100755 blob " + packaging + b"\tpackaging/retrode3/build-debs.sh",
    )) + b"\0"


class KernelCacheTests(unittest.TestCase):
    def test_driver_only_preserves_base(self):
        old = cache.keys(tree(), IMAGE)
        new = cache.keys(tree(driver=b"two"), IMAGE)
        self.assertEqual(old[0], new[0])
        self.assertNotEqual(old[1], new[1])

    def test_core_kconfig_toolchain_change_base(self):
        old = cache.keys(tree(), IMAGE)[0]
        self.assertNotEqual(old, cache.keys(tree(kernel=b"new"), IMAGE)[0])
        self.assertNotEqual(old, cache.keys(tree(kconfig=b"new"), IMAGE)[0])
        self.assertNotEqual(old, cache.keys(tree(), "sha256:" + "b" * 64)[0])

    def test_packaging_only_preserves_compile_cache(self):
        self.assertEqual(cache.keys(tree(), IMAGE),
                         cache.keys(tree(packaging=b"new"), IMAGE))

    def test_unsafe_isolation_disabled(self):
        self.assertNotEqual(cache.keys(tree(), IMAGE, False)[0],
                            cache.keys(tree(driver=b"two"), IMAGE, False)[0])


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Exercise the exact postinst logic with temporary files and fake device commands.
Never mounts or touches an actual boot filesystem.
"""
import os
import re
import subprocess
import tempfile
from pathlib import Path
import unittest

SCRIPT = Path(__file__).with_name("kernel-postinst.sh")
VERSION = "7.3.0~rc5+build1-1"
RELEASE = "7.3.0-rc5-letux-mips-retrode3+r3.abcdef123456"


class InstallTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = r = Path(temp.name)
        self.bin = r / "bin"
        self.bin.mkdir()
        self.log = r / "mountlog"
        self.boot = r / "boot"
        (self.boot / "ingenic").mkdir(parents=True)
        (r / "run").mkdir()
        (r / "dev").mkdir()
        payload = r / "usr/lib/retrode3/kernel-candidates" / VERSION
        (payload / "boot/ingenic").mkdir(parents=True)
        (payload / "kernel-release").write_text(RELEASE + "\n")
        (payload / "boot/uImage").write_bytes(b"new kernel")
        (payload / "boot/ingenic/lx16-retrode-2.9.5.dtb").write_bytes(b"new dtb")
        modules = r / "lib/modules" / RELEASE
        ko = modules / "kernel/drivers/misc/retrode3/retrode3.ko"
        ko.parent.mkdir(parents=True)
        ko.write_bytes(b"fixture")
        (modules / "modules.dep").touch()
        for i in (1, 2):
            (r / ("dev/mmcblk0p%d" % i)).touch()
        (self.boot / "uImage").write_bytes(b"old kernel")
        (self.boot / "bootargs.scr").write_bytes(b"unchanged bootargs")
        script = re.sub(
            r"(?<![\w/])(/usr/lib/retrode3/kernel-candidates|/lib/modules|/dev/mmcblk0p[12]|/boot|/run/retrode3-kernel-reboot-required)",
            lambda m: str(r) + m[1], SCRIPT.read_text())
        script = script.replace("[ -b ", "[ -f ")
        self.script = r / "test.sh"
        self.script.write_text(script)
        self.mock("dpkg-query", 'case "$*" in *linux-image) echo "%s";; *) echo "installed ${MOCK_MODULE_VERSION:-%s}";; esac'.replace("\\$", "$") % (VERSION, VERSION))
        self.mock("modinfo", 'echo "${MOCK_VERMAGIC:-%s preempt mod_unload modversions MIPS32_R2 32BIT}"'.replace("\\$", "$") % RELEASE)
        self.mock("lsblk", 'case "$*" in *mmcblk0p1) echo 179:1;; *) echo 179:2;; esac')
        self.mock("findmnt", """case "$*" in
          *"-M / -o MAJ:MIN") echo "${MOCK_ROOT_ID:-179:2}";;
          *"-o MAJ:MIN") echo "${MOCK_BOOT_ID:-179:1}";;
          *"-o FSTYPE") echo vfat;;
          *"-o OPTIONS") echo "${MOCK_OPTIONS:-rw,nosuid}";;
          *) test -f "%s/mounted";;
        esac""".replace("\\$", "$") % r)
        self.mock("mount", 'echo mount >> "%s"; touch "%s/mounted"' % (self.log, r))
        self.mock("umount", 'echo umount >> "%s"; rm -f "%s/mounted"' % (self.log, r))
        self.mock("sync", ":")
        self.mock("uname", "echo old-kernel")

    def mock(self, name, body):
        path = self.bin / name
        path.write_text("#!/bin/sh\nset -eu\n" + body + "\n")
        path.chmod(0o755)

    def attempt(self, **env):
        return subprocess.run(["sh", str(self.script), "configure"],
                              env=dict(os.environ, PATH=str(self.bin) + ":" + os.environ["PATH"], **env),
                              capture_output=True, text=True)

    def test_offline_never_mounts(self):
        result = self.attempt(RETRODE3_OFFLINE_IMAGE="1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.log.exists())
        self.assertEqual((self.boot / "uImage").read_bytes(), b"old kernel")

    def test_wrong_version_abi_and_root_refused(self):
        for env, message in (({"MOCK_MODULE_VERSION": "wrong"}, "matching modules"),
                             ({"MOCK_VERMAGIC": "wrong"}, "ABI mismatch"),
                             ({"MOCK_ROOT_ID": "8:1"}, "root filesystem")):
            with self.subTest(env=env):
                result = self.attempt(**env)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)
                self.assertFalse(self.log.exists())

    def test_wrong_boot_partition_refused(self):
        result = self.attempt(MOCK_BOOT_ID="8:2")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("expected FAT", result.stderr)
        self.assertEqual(self.log.read_text(), "mount\numount\n")
        self.assertEqual((self.boot / "uImage").read_bytes(), b"old kernel")

    def test_normal_install(self):
        result = self.attempt()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.log.read_text(), "mount\numount\n")
        self.assertEqual((self.boot / "uImage").read_bytes(), b"new kernel")
        for dtb in ("lx16.dtb", "lx16-retrode-2.9.5.dtb"):
            self.assertEqual((self.boot / "ingenic" / dtb).read_bytes(), b"new dtb")
        self.assertEqual((self.boot / "bootargs.scr").read_bytes(), b"unchanged bootargs")
        self.assertTrue((self.root / "run/retrode3-kernel-reboot-required").is_file())


if __name__ == "__main__":
    unittest.main()

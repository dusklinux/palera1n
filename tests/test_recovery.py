import importlib.util
import io
import json
from pathlib import Path
import plistlib
import subprocess
import tarfile
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("recovery", ROOT / "recovery/recovery.py")
recovery = importlib.util.module_from_spec(spec)
spec.loader.exec_module(recovery)


class LocalRemote:
    """Exercise the actual recovery shell commands against a disposable directory."""
    def __init__(self):
        self.commands = []

    def command(self, command, *, data=None, timeout=90):
        self.commands.append(command)
        # No system-wide sync is necessary for a disposable fixture.
        command = command.replace("; sync", "; :")
        result = subprocess.run(["sh", "-c", command], input=data, capture_output=True, timeout=timeout)
        if result.returncode:
            raise recovery.RecoveryError(result.stderr.decode())
        return result.stdout


class RecoveryTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="recovery test ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.database = self.root / "remote volume" / "com.apple.xpc.launchd"
        self.database.mkdir(parents=True)
        self.backup = self.root / "backup"
        self.backup.mkdir()
        self.remote = LocalRemote()
        (self.database / "disabled.plist").write_bytes(plistlib.dumps({"test.system": True}))
        (self.database / "disabled.501.plist").write_bytes(plistlib.dumps({"test.user": True}))
        (self.database / "disabled.migrated").write_bytes(b"")
        (self.database / "disabled.backup").write_bytes(b"preserve old backup")

    def backup_files(self):
        return recovery.backup_overrides(self.remote, self.backup, str(self.database))

    def test_reset_includes_user_domain_and_preserves_original_bytes(self):
        files = self.backup_files()
        self.assertEqual(set(files), {"disabled.plist", "disabled.501.plist", "disabled.migrated"})
        destination = Path(recovery.reset_overrides(self.remote, files, "test-stamp", str(self.database)))
        for name, data in files.items():
            self.assertFalse((self.database / name).exists())
            self.assertEqual((destination / name).read_bytes(), data)
        self.assertEqual((self.database / "disabled.backup").read_bytes(), b"preserve old backup")
        self.assertTrue((self.backup / "launchd-before.tar").is_file())

    def test_backup_failure_cannot_change_phone_files(self):
        with patch.object(self.remote, "command", side_effect=recovery.RecoveryError("tar failed")):
            with self.assertRaises(recovery.RecoveryError):
                self.backup_files()
        self.assertTrue((self.database / "disabled.plist").exists())
        self.assertFalse((self.root / "remote volume/palera1n-recovery").exists())

    def test_changed_snapshot_is_rejected_before_reset(self):
        original = self.remote.command
        def changed(command, **kwargs):
            if command.startswith("cat "):
                return b"changed"
            return original(command, **kwargs)
        with patch.object(self.remote, "command", side_effect=changed):
            with self.assertRaisesRegex(recovery.RecoveryError, "changed during backup"):
                self.backup_files()
        self.assertTrue((self.database / "disabled.plist").exists())

    def test_corrupt_override_can_still_be_backed_up_and_reset(self):
        (self.database / "disabled.plist").write_bytes(b"broken plist")
        files = self.backup_files()
        self.assertEqual(files["disabled.plist"], b"broken plist")
        recovery.reset_overrides(self.remote, files, "corrupt", str(self.database))
        self.assertFalse((self.database / "disabled.plist").exists())

    def test_failed_move_is_not_reported_as_verified(self):
        files = self.backup_files()
        (self.database / "disabled.501.plist").unlink()
        with self.assertRaises(recovery.RecoveryError):
            recovery.reset_overrides(self.remote, files, "failed", str(self.database))
        self.assertTrue((self.backup / "launchd-before.tar").is_file())

    def test_unexpected_remaining_user_override_fails_verification(self):
        files = self.backup_files()
        (self.database / "disabled.502.plist").write_bytes(b"unexpected")
        with self.assertRaisesRegex(recovery.RecoveryError, "override remains"):
            recovery.reset_overrides(self.remote, files, "remaining", str(self.database))

    def test_restore_is_byte_exact_and_preserves_current_backup(self):
        files = self.backup_files()
        recovery.reset_overrides(self.remote, files, "first", str(self.database))
        current = {"disabled.plist": plistlib.dumps({"new": False})}
        (self.database / "disabled.plist").write_bytes(current["disabled.plist"])
        destination = Path(recovery.restore_overrides(self.remote, files, current, "rollback", str(self.database)))
        for name, data in files.items():
            self.assertEqual((self.database / name).read_bytes(), data)
            self.assertEqual((self.database / name).stat().st_mode & 0o777, 0o644)
        self.assertEqual((destination / "disabled.plist").read_bytes(), current["disabled.plist"])

    def test_duplicate_archive_members_rejected(self):
        archive = io.BytesIO()
        with tarfile.open(fileobj=archive, mode="w") as output:
            for _ in range(2):
                info = tarfile.TarInfo("com.apple.xpc.launchd/disabled.plist")
                output.addfile(info, io.BytesIO())
        with self.assertRaises(recovery.RecoveryError):
            recovery.read_overrides(archive.getvalue())

    def test_device_identity_checks_board_and_chip(self):
        manifest = {"product": "iPhone10,2", "board": "d21ap", "cpid": "0x8015"}
        info = {"PRODUCT": "iPhone10,2", "MODEL": "d21ap", "CPID": "0x8015"}
        self.assertTrue(recovery.matching_device(info, manifest))
        self.assertFalse(recovery.matching_device(dict(info, MODEL="d211ap"), manifest))

    def test_bundle_checksums_and_missing_files(self):
        bundle = self.root / "bundle"
        bundle.mkdir()
        manifest = {"format": 1, "product": "test", "board": "test", "cpid": "0x8015",
                    "ramdisk_version": "16.0", "usb_serial_prefix": "SSHRD_Script", "sha256": {}}
        for name in (*recovery.IMAGES, "gaster"):
            path = bundle / name
            path.write_bytes(b"fixture")
            path.chmod(0o755)
            manifest["sha256"][name] = recovery.hashlib.sha256(b"fixture").hexdigest()
        (bundle / "manifest.json").write_text(json.dumps(manifest))
        self.assertEqual(recovery.load_bundle(bundle)["product"], "test")
        (bundle / "ramdisk.img4").write_bytes(b"truncated")
        with self.assertRaisesRegex(recovery.RecoveryError, "checksum mismatch"):
            recovery.load_bundle(bundle)
        (bundle / "ramdisk.img4").unlink()
        with self.assertRaisesRegex(recovery.RecoveryError, "Incomplete"):
            recovery.load_bundle(bundle)


if __name__ == "__main__":
    unittest.main()

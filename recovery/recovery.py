"""Offline SSHRD recovery; embedded in the Linux palera1n executable.

Normal iOS and its OpenSSH/lockdownd services are never used for this operation.
"""
import argparse
import contextlib
import datetime
import hashlib
import io
import json
import os
from pathlib import Path
import plistlib
import pwd
import re
import shlex
import shutil
import socket
import subprocess
import sys
import tarfile
import tempfile
import time

IMAGES = ("iBSS.img4", "iBEC.img4", "logo.img4", "ramdisk.img4",
          "devicetree.img4", "trustcache.img4", "kernelcache.img4")
DATABASE = "/mnt2/db/com.apple.xpc.launchd"


class RecoveryError(RuntimeError):
    pass


def run(args, *, data=None, timeout=120, check=True):
    result = subprocess.run([str(a) for a in args], input=data, capture_output=True, timeout=timeout)
    if check and result.returncode:
        raise RecoveryError(f"{args[0]} exited {result.returncode}: "
                            + (result.stderr or result.stdout).decode(errors="replace")[-2000:])
    return result


def load_bundle(directory):
    directory = directory.resolve(strict=True)
    manifest = json.loads((directory / "manifest.json").read_text(encoding="utf-8"))
    if manifest.get("format") != 1:
        raise RecoveryError("Unsupported recovery bundle format")
    for key in ("product", "board", "cpid", "ramdisk_version", "usb_serial_prefix", "sha256"):
        if not manifest.get(key):
            raise RecoveryError(f"Recovery bundle is missing {key}")
    for name in (*IMAGES, "gaster"):
        path = directory / name
        if name not in manifest["sha256"] or not path.is_file():
            raise RecoveryError(f"Incomplete recovery bundle: {name}")
        with path.open("rb") as source:
            digest = hashlib.file_digest(source, "sha256").hexdigest()
        if digest != manifest["sha256"][name]:
            raise RecoveryError(f"Recovery bundle checksum mismatch: {name}")
    if not os.access(directory / "gaster", os.X_OK):
        raise RecoveryError("The bundle's gaster executable is not executable")
    return manifest


def parse_info(output):
    return dict(line.split(": ", 1) for line in output.decode().splitlines() if ": " in line)


def matching_device(info, manifest):
    return (info.get("PRODUCT") == manifest["product"] and
            info.get("MODEL") == manifest["board"] and
            int(info.get("CPID", "0"), 0) == int(manifest["cpid"], 0))


def boot_ramdisk(bundle, manifest, executable):
    result = run(["irecovery", "-q"], timeout=10, check=False)
    if result.returncode:
        raise RecoveryError("Enter recovery or DFU mode first. No phone was changed.")
    info = parse_info(result.stdout)
    if not matching_device(info, manifest):
        raise RecoveryError("Connected device does not match this recovery bundle")
    ecid = info["ECID"]
    if info.get("MODE") != "DFU":
        print("Follow the DFU helper. This does not boot installed iOS.", flush=True)
        subprocess.run([executable, "-D"], check=True)
        info = parse_info(run(["irecovery", "-i", ecid, "-q"], timeout=10).stdout)
        if info.get("MODE") != "DFU" or not matching_device(info, manifest):
            raise RecoveryError("The expected device did not enter DFU")
    print("Booting the standalone SSH ramdisk…", flush=True)
    print("Exploiting DFU…", flush=True)
    run([bundle / "gaster", "pwn"])
    print("Resetting DFU transport…", flush=True)
    run([bundle / "gaster", "reset"])
    def send(name):
        print(f"Uploading {name}…", flush=True)
        run(["irecovery", "-i", ecid, "-f", bundle / name], timeout=180)
    def command(value):
        run(["irecovery", "-i", ecid, "-c", value])
    send("iBSS.img4")
    time.sleep(2)
    send("iBEC.img4")
    if int(manifest["cpid"], 0) in (0x8010, 0x8011, 0x8012, 0x8015):
        command("go")
    time.sleep(2)
    send("logo.img4")
    command("setpicture 0x1")
    send("ramdisk.img4")
    command("ramdisk")
    send("devicetree.img4")
    command("devicetree")
    send("trustcache.img4")
    command("firmware")
    send("kernelcache.img4")
    command("bootx")
    return ecid


def ramdisk_serial(prefix, root=Path("/sys/bus/usb/devices")):
    found = []
    for device in root.iterdir():
        try:
            if (device / "idVendor").read_text().strip() != "05ac":
                continue
            serial = (device / "serial").read_text().strip()
            if serial.startswith(prefix):
                found.append(serial)
        except OSError:
            continue
    if len(found) > 1:
        raise RecoveryError("More than one SSHRD device is connected")
    return found[0] if found else None


def usbmux_ready():
    try:
        with socket.socket(socket.AF_UNIX) as sock:
            sock.connect("/var/run/usbmuxd")
        return True
    except OSError:
        return False


def stop_process(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


class Remote:
    def __init__(self, port, directory):
        self.base = ["ssh", "-p", str(port), "-o", "ConnectTimeout=5",
                     "-o", "ServerAliveInterval=3", "-o", "ServerAliveCountMax=3",
                     "-o", "StrictHostKeyChecking=accept-new", "-o",
                     f"UserKnownHostsFile={directory / 'known_hosts'}",
                     "-o", "ControlMaster=auto", "-o", "ControlPersist=60",
                     "-S", str(directory / "ssh"), "root@127.0.0.1"]

    def connect(self):
        environment = dict(os.environ, SSHPASS="alpine")
        result = subprocess.run(["sshpass", "-e", *self.base, "uname -m; command -v mount_filesystems"],
                                capture_output=True, timeout=12, env=environment)
        if result.returncode or b"/usr/bin/mount_filesystems" not in result.stdout:
            raise RecoveryError("SSHRD authentication/identity check failed: "
                                + result.stderr.decode(errors="replace")[-1000:])

    def command(self, command, *, data=None, timeout=90):
        return run([*self.base[:-1], "-o", "BatchMode=yes", self.base[-1], command],
                   data=data, timeout=timeout).stdout

    def close(self):
        with contextlib.suppress(OSError, subprocess.SubprocessError):
            run([*self.base[:-1], "-O", "exit", self.base[-1]], check=False, timeout=10)


@contextlib.contextmanager
def connection(serial, directory, logs):
    with contextlib.ExitStack() as stack:
        if not usbmux_ready():
            log = stack.enter_context((logs / "usbmuxd.log").open("wb"))
            daemon = subprocess.Popen(["usbmuxd", "-f", "-p"], stdout=log, stderr=log)
            stack.callback(stop_process, daemon)
            deadline = time.monotonic() + 10
            while not usbmux_ready():
                if daemon.poll() is not None or time.monotonic() >= deadline:
                    raise RecoveryError("usbmuxd failed; see usbmuxd.log")
                time.sleep(.1)
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        log = stack.enter_context((logs / "iproxy.log").open("wb"))
        # A fresh, explicitly targeted proxy avoids stale usbmux device handles.
        proxy = subprocess.Popen(["iproxy", "-u", serial, "-s", "127.0.0.1", f"{port}:22"],
                                 stdout=log, stderr=log)
        stack.callback(stop_process, proxy)
        deadline = time.monotonic() + 30
        while True:
            if proxy.poll() is not None:
                raise RecoveryError("iproxy exited; see iproxy.log")
            try:
                with socket.create_connection(("127.0.0.1", port), 1) as sock:
                    sock.settimeout(1)
                    if sock.recv(128).startswith(b"SSH-"):
                        break
            except OSError:
                pass
            if time.monotonic() >= deadline:
                raise RecoveryError("The SSHRD SSH server did not become ready")
            time.sleep(.2)
        remote = Remote(port, directory)
        remote.connect()
        stack.callback(remote.close)
        yield remote


def override_name(name):
    return name == "disabled.migrated" or re.fullmatch(r"disabled(?:\.\d+)?\.plist", name) is not None


def read_overrides(archive):
    """Only inspect the archive; do not extract paths onto the laptop."""
    result = {}
    with tarfile.open(fileobj=io.BytesIO(archive)) as source:
        for item in source:
            parts = Path(item.name).parts
            if len(parts) == 2 and parts[0] == "com.apple.xpc.launchd" and override_name(parts[1]):
                if not item.isfile() or parts[1] in result:
                    raise RecoveryError("Invalid launchd backup member")
                result[parts[1]] = source.extractfile(item).read()
    return result


def backup_overrides(remote, directory, database=DATABASE):
    parent = str(Path(database).parent)
    archive = remote.command(f"tar -cf - -C {shlex.quote(parent)} com.apple.xpc.launchd")
    files = read_overrides(archive)
    # No changes until the archive is durable and each live file matches it.
    target = directory / "launchd-before.tar"
    with target.open("xb") as output:
        output.write(archive)
        output.flush()
        os.fsync(output.fileno())
    for name, data in files.items():
        actual = remote.command(f"cat {shlex.quote(database + '/' + name)}")
        if actual != data:
            raise RecoveryError("The launchd database changed during backup; nothing reset")
    return files


def reset_overrides(remote, files, stamp, database=DATABASE):
    destination = str(Path(database).parent / "palera1n-recovery" / stamp)
    commands = ["set -e", f"mkdir -p {shlex.quote(destination)}"]
    for name in files:
        commands.append(f"mv {shlex.quote(database + '/' + name)} {shlex.quote(destination + '/' + name)}")
    commands.append("sync")
    for name in files:
        commands.append(f"test ! -e {shlex.quote(database + '/' + name)}")
    remote.command("; ".join(commands))
    # Independently confirm no per-user override was missed.
    remaining = remote.command(f"ls -1 {shlex.quote(database)}").decode().splitlines()
    if any(override_name(name) for name in remaining):
        raise RecoveryError("A launchd override remains; reset NOT verified")
    return destination


def restore_overrides(remote, original, current, stamp, database=DATABASE):
    destination = reset_overrides(remote, current, stamp, database)
    for name, data in original.items():
        target = database + "/" + name
        temp = target + ".recovery-new"
        remote.command(f"set -e; umask 022; cat > {shlex.quote(temp)}; chmod 0644 {shlex.quote(temp)}; "
                       f"mv {shlex.quote(temp)} {shlex.quote(target)}; sync", data=data)
        if remote.command(f"cat {shlex.quote(target)}") != data:
            raise RecoveryError(f"Restore verification failed for {name}")
    return destination


def write_report(path, report):
    temporary = path.with_suffix(".json.tmp")
    with temporary.open("w", encoding="utf-8") as output:
        json.dump(report, output, indent=2)
        output.write("\n")
        output.flush()
        os.fsync(output.fileno())
    temporary.replace(path)


def default_backup_directory():
    uid = int(os.environ.get("SUDO_UID", os.getuid()))
    return Path(pwd.getpwuid(uid).pw_dir) / "Documents" / "palera1n-recovery"


def main(argv=None):
    parser = argparse.ArgumentParser(prog="palera1n --recover-daemons",
        description="Boot an offline SSHRD bundle, back up and inspect launchd overrides. "
                    "Reset/restore changes only override files; it cannot guarantee a working iOS boot.")
    parser.add_argument("--bundle", type=Path, required=True)
    parser.add_argument("--validate-bundle", action="store_true", help="check bundle files only; do not touch USB")
    action = parser.add_mutually_exclusive_group()
    action.add_argument("--reset-overrides", action="store_true", help="restore native launchd defaults after a verified backup")
    action.add_argument("--restore", type=Path, help="restore override files from a previous recovery backup directory")
    parser.add_argument("--backup-dir", type=Path, default=default_backup_directory())
    parser.add_argument("--reboot", action="store_true", help="reboot after the requested operation; default stays in SSHRD")
    args = parser.parse_args(argv)
    manifest = load_bundle(args.bundle)
    if args.validate_bundle:
        print(f"Verified offline bundle for {manifest['product']} ({manifest['board']}), ramdisk {manifest['ramdisk_version']}")
        return 0
    if sys.platform != "linux" or os.geteuid() != 0:
        raise RecoveryError("Run recovery on Linux with sudo")
    for command in ("irecovery", "iproxy", "usbmuxd", "ssh", "sshpass"):
        if not shutil.which(command):
            raise RecoveryError(f"Missing host command: {command}")
    executable = os.environ.get("PALERA1N_EXECUTABLE")
    if not executable:
        raise RecoveryError("Run this helper through palera1n --recover-daemons")
    original = None
    restore_identity = None
    if args.restore:
        restore_identity = json.loads((args.restore / "report.json").read_text(encoding="utf-8"))
        original = read_overrides((args.restore / "launchd-before.tar").read_bytes())
    stamp = datetime.datetime.now(datetime.UTC).strftime("%Y%m%dT%H%M%S.%fZ")
    directory = args.backup_dir.expanduser().resolve() / stamp
    directory.mkdir(parents=True)
    print(f"Backups and report: {directory}", flush=True)
    ecid = boot_ramdisk(args.bundle.resolve(), manifest, executable)
    if restore_identity and int(restore_identity["ecid"], 0) != int(ecid, 0):
        raise RecoveryError("The restore backup belongs to a different phone")
    deadline = time.monotonic() + 90
    print("Waiting for the independent SSHRD USB connection…", flush=True)
    while not (serial := ramdisk_serial(manifest["usb_serial_prefix"])):
        if time.monotonic() >= deadline:
            raise RecoveryError("The SSH ramdisk did not enumerate; no overrides were changed")
        time.sleep(.2)
    report = {"ecid": ecid, "product": manifest["product"],
              "action": "reset-overrides" if args.reset_overrides else "restore-overrides" if args.restore else "inspect",
              "verified": False,
              "usable_ios_boot_verified": False}
    report_path = directory / "report.json"
    write_report(report_path, report)
    with tempfile.TemporaryDirectory(prefix="palera1n-recovery-") as temporary:
        with connection(serial, Path(temporary), directory) as remote:
            print("SSHRD connected. Mounting filesystems…", flush=True)
            remote.command("/usr/bin/mount_filesystems", timeout=120)
            installed = plistlib.loads(remote.command("cat /mnt1/System/Library/CoreServices/SystemVersion.plist"))
            report["ios_version"] = installed["ProductVersion"]
            report["ios_build"] = installed["ProductBuildVersion"]
            remote.command(f"test -d {DATABASE} && test -d /mnt2/mobile")
            current = backup_overrides(remote, directory)
            report["override_files"] = list(current)
            counts = {}
            for name, data in current.items():
                if not name.endswith(".plist"):
                    continue
                try:
                    values = plistlib.loads(data)
                    counts[name] = sum(value is True for value in values.values())
                except (ValueError, plistlib.InvalidFileException, AttributeError):
                    counts[name] = "unreadable (original bytes backed up)"
            report["disabled_before"] = counts
            write_report(report_path, report)
            # The existing archive is backed up before touching a single override.
            if args.reset_overrides:
                report["device_backup"] = reset_overrides(remote, current, stamp)
                report["action"] = "reset-overrides"
            elif original is not None:
                report["device_backup"] = restore_overrides(remote, original, current, stamp)
                report["action"] = "restore-overrides"
            report["verified"] = True
            write_report(report_path, report)
            print(json.dumps(report, indent=2), flush=True)
            if args.reboot:
                # Save verification before intentionally losing the connection.
                remote.command("sync; /sbin/reboot")
                print("Reboot requested. A usable iOS boot still needs to be checked.", flush=True)
            else:
                print("Phone left in SSHRD. No automatic reboot.", flush=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RecoveryError, OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"Recovery failed: {error}", file=sys.stderr)
        raise SystemExit(1)

# Offline daemon recovery on Arch Linux x86_64

This is a standalone SSH ramdisk workflow, not safe mode or an SSH connection to
installed iOS. It works independently of installed OpenSSH, lockdownd and the
installed launchd overrides. It requires a working checkm8/USB path and a ramdisk
that can boot and mount the installed Data volume. It cannot guarantee recovery
from every bootloop, filesystem failure, SEP problem, or hardware fault.

## Prepare before changing services

Keep the binary and recovery bundle on persistent storage. The normal jailbreak
ramdisk embedded in palera1n is **not** the standalone SSHRD recovery bundle.
The recovery command does not download firmware or build a ramdisk during an emergency.

1. Use [SSHRD_Script](https://github.com/verygenericname/SSHRD_Script) to build a
   ramdisk for the actual device. On Linux its documented HFS workflow uses 16.0
   rather than 16.1+ APFS ramdisks. This does not install or downgrade iOS.
2. Record `PRODUCT`, `MODEL`, and `CPID` from `irecovery -q` on that device.
3. Export the finished bundle. This example corresponds to an iPhone 8 Plus;
   substitute the **actual build's** identifiers for another device:

   ```sh
   python3 tools/make-recovery-bundle.py \
     --source /path/to/SSHRD_Script --output /persistent/path/recovery-bundle \
     --product iPhone10,2 --board d21ap --cpid 0x8015
   ./build/palera1n-linux-x86_64 --recover-daemons \
     --bundle /persistent/path/recovery-bundle --validate-bundle
   ```

4. Test booting the bundle and mounting Data while the phone is healthy. The
   inspect command below makes a host backup and leaves the phone in SSHRD.
   Retest after an iOS update; a hash check verifies file integrity, not SEP/APFS compatibility.

Host recovery dependencies: Python 3.15+, `irecovery`, `iproxy`, `usbmuxd`, OpenSSH,
and `sshpass`. The bundle includes gaster and the seven prepared image files.
It contains no user's data or phone backup. Preserve the relevant upstream licenses
when redistributing third-party binaries/firmware; these assets are not committed here.

## Inspect and back up

Connect the phone in recovery or DFU mode. If necessary, the command runs this
palera1n binary's DFU helper before booting the independent ramdisk:

```sh
sudo ./build/palera1n-linux-x86_64 --recover-daemons --bundle /persistent/path/recovery-bundle
```

The helper verifies the product/board/chip, waits for the SSHRD USB identity,
starts a temporary usbmuxd only if none is running, and creates a fresh, targeted
USB tunnel. It authenticates to the known SSHRD image with its default `alpine`
password, mounts filesystems, and backs up the launchd directory. Installed iOS's
SSH credentials and disabled services are not used.

Backups default to the invoking user's `~/Documents/palera1n-recovery/TIMESTAMP`.
Use `--backup-dir PATH` to select another persistent location. The archive is
flushed to disk, and each active override is read back and compared to the archive
before reset is allowed. An error returns a nonzero exit status and is not a verified repair.

## Reset saved launchd overrides

```sh
sudo ./build/palera1n-linux-x86_64 --recover-daemons \
  --bundle /persistent/path/recovery-bundle --reset-overrides
```

This moves `disabled.plist`, numbered per-user files such as `disabled.501.plist`,
and `disabled.migrated` into a timestamped directory under
`/var/db/palera1n-recovery/` on the phone. It verifies that none remain active and
runs `sync`. Existing unrelated files and backups are preserved. It restores
native defaults rather than explicitly enabling jobs whose stock default is disabled.
It does not erase user data, remove the jailbreak, or apply Daemon Manager presets.

Default behavior stays in SSHRD for inspection. Add `--reboot` only when ready.
`report.json` records the original disabled counts, installed iOS/build, ECID,
operation and verification result. `usable_ios_boot_verified` remains false:
a successful file reset is not proof that iOS boots or every daemon runs.

## Roll back a recovery change

```sh
sudo ./build/palera1n-linux-x86_64 --recover-daemons \
  --bundle /persistent/path/recovery-bundle --restore /path/to/previous/TIMESTAMP
```

The previous directory must contain `report.json` and `launchd-before.tar`.
The ECID must match. The current files are backed up first, then the archived
active override files are restored atomically per file with `root:wheel 0644`
metadata and byte-for-byte readback. A rollback can restore disabled services;
it is an explicit operation, never an automatic reaction to a boot failure.

## USB behavior and limits

The Linux PongoOS path enumerates before waiting, retries transient failures only
before uploading commands, and holds the selected USB device's runtime power
control at `on`. It restores the previous `auto` setting only if the same USB
device address still occupies that port. No global USB power policy or persistent
udev rule is changed. Transfers use finite timeouts; failed uploads stop the
sequence and produce a nonzero exit status. Partially sent boot sequences are
not blindly replayed.

The exploit itself and physical cable/controller behavior can still fail. A
"kernel handoff sent" message confirms only the handoff attempt. A useful recovery
kit is a tested escape path for override problems, not a promise of an unbrickable phone.

## Rebuild the Linux binary

Run `tools/build-linux.sh --stress` from the checkout. Subsequent builds can use
`tools/build-linux.sh --offline --clean --stress` with installed dependencies and
cached payloads. `--output /persistent/path` selects the packaged output directory;
otherwise it is `build/ready`. The executable links the current Arch system
libraries. Keep those dependencies installed when copying the binary elsewhere.
The SSHRD bundle is separate and must also be kept on persistent storage.
See [VALIDATION.md](VALIDATION.md) for the exercised cases and hardware limits.

Build payload hashes are tracked in `tools/resources.sha256`. Downloads are staged
before entering the cache, and every build checks cached bytes. When intentionally
updating a payload, review the corresponding upstream change and update its hash;
a changed CDN file will stop the build instead of silently changing the executable.

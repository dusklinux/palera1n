#!/usr/bin/env python3
"""Export an already built SSHRD_Script ramdisk for offline recovery."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

IMAGES = ("iBSS.img4", "iBEC.img4", "logo.img4", "ramdisk.img4",
          "devicetree.img4", "trustcache.img4", "kernelcache.img4")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path, help="SSHRD_Script checkout with a finished sshramdisk directory")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--product", required=True, help="PRODUCT from irecovery -q when these images were built")
    parser.add_argument("--board", required=True, help="MODEL from the same device")
    parser.add_argument("--cpid", required=True, help="CPID from the same device")
    args = parser.parse_args()
    source = args.source.resolve(strict=True)
    files = {name: source / "sshramdisk" / name for name in IMAGES}
    files["gaster"] = source / "Linux" / "gaster"
    for name, path in files.items():
        if not path.is_file() or path.stat().st_size == 0:
            parser.error(f"Missing or empty source asset: {path}")
    manifest = {"format": 1, "product": args.product, "board": args.board,
                "cpid": hex(int(args.cpid, 0)), "usb_serial_prefix": "SSHRD_Script",
                "ramdisk_version": (source / "sshramdisk/version.txt").read_text(encoding="utf-8").strip(),
                "source": "https://github.com/verygenericname/SSHRD_Script",
                "sha256": {}}
    args.output.mkdir(parents=True, exist_ok=False)
    for name, path in files.items():
        destination = args.output / name
        shutil.copyfile(path, destination)
        destination.chmod(0o755 if name == "gaster" else 0o644)
        with destination.open("rb") as stream:
            manifest["sha256"][name] = hashlib.file_digest(stream, "sha256").hexdigest()
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Created offline bundle: {args.output}")
    print("Preserve it before restoring/changing iOS. Its ability to mount Data must be tested on the target build.")


if __name__ == "__main__":
    main()

#!/usr/bin/env bash
# Native Linux x86_64 build, verification, and persistent output package.
set -euo pipefail
cd -- "$(dirname -- "$(realpath -- "${BASH_SOURCE[0]}")")/.."
jobs=$(nproc)
output=build/ready
offline=0
clean=0
stress=0
while (($#)); do
    case $1 in
        --jobs|--output)
            (($# >= 2)) || { echo "Missing value for $1" >&2; exit 2; }
            if [[ $1 == --jobs ]]; then jobs=$2; else output=$2; fi
            shift 2 ;;
        --offline) offline=1; shift ;;
        --clean) clean=1; shift ;;
        --stress) stress=1; shift ;;
        --help)
            echo 'Usage: tools/build-linux.sh [--jobs N] [--output DIR] [--offline] [--clean] [--stress]'
            echo 'Builds Linux x86_64, runs tests, and packages the binary, checksums and build versions.'
            echo 'Offline requires cached src/resources payloads and installed build dependencies.'
            exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done
[[ $(uname -s)-$(uname -m) == Linux-x86_64 ]] || { echo 'Requires Linux x86_64' >&2; exit 1; }
[[ $jobs =~ ^[1-9][0-9]*$ ]] || { echo 'Jobs must be a positive integer' >&2; exit 2; }
for command in cc make pkg-config python3 xxd xz curl strip install sha256sum git; do
    command -v "$command" >/dev/null || { echo "Missing dependency: $command" >&2; exit 1; }
done
python3 -c 'import sys; sys.exit(0 if sys.version_info[:2] >= (3, 15) else "Python 3.15+ required")'
if ((clean)); then make -f Makefile.arch clean; fi
make -f Makefile.arch -j"$jobs" OFFLINE="$offline"
make -f Makefile.arch test
if ((stress)); then python3 tools/stress-linux.py; fi
mkdir -p -- "$output"
install -m755 build/palera1n-linux-x86_64 "$output/palera1n-linux-x86_64"
strip --strip-unneeded "$output/palera1n-linux-x86_64"
install -m644 docs/RECOVERY.md "$output/RECOVERY.md"
{
    git describe --always --dirty --tags
    git rev-parse HEAD
    cc --version
    python3 --version
    pkg-config --modversion libusb-1.0 libimobiledevice-1.0 libirecovery-1.0 libusbmuxd-2.0 libplist-2.0
} > "$output/build-info.txt"
(cd -- "$output" && sha256sum palera1n-linux-x86_64 RECOVERY.md build-info.txt > SHA256SUMS)
"$output/palera1n-linux-x86_64" --version
printf 'Verified build written to %s\n' "$(realpath -- "$output")"

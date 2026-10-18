# Validation on the development host

Tested on Arch Linux x86_64 with GCC 16.2.1, libusb 1.0.30, and Python 3.15.0rc3.
These observations describe this build and hardware, not a guarantee for other phones.

- Ten recovery tests passed, including failed backup/readback, partial reset failure,
  corrupt plist preservation, system/user overrides, and byte-exact rollback.
- Stress run: 100 USB suites, 100 Pongo suites, 20 recovery suites (200 test cases),
  and 10 exploit-helper suites; all passed in 36.33 seconds.
- USB, Pongo, and exploit-helper suites also passed AddressSanitizer and
  UndefinedBehaviorSanitizer with leak detection enabled.
- Actual iPhone 8 Plus: DFU exploit and complete Pongo image upload succeeded twice
  without unplugging. Pongo discovery took two seconds after its boot message.
- An already suspended Pongo connection reproduced `LIBUSB_ERROR_IO`; the new
  binary retried discovery and opened it one second later without intervention.
- Independent recovery ramdisk booted and mounted installed iOS 16.7.16 Data.
  Integrated backup/reset/readback completed. Active native overrides were already
  absent in that particular test, so a separate disposable directory on the phone
  exercised three nonempty override files: reset and rollback preserved exact
  bytes and root:wheel 0644 metadata. The active database was unchanged by that fixture.
- Resetting the original phone's launchd overrides did **not** cure its existing
  bootloop. A successful override reset is deliberately not reported as a usable
  iOS boot. The current bootloop's root cause remains undiagnosed.

Reproduce host checks with `tools/build-linux.sh --offline --stress` after the
payload cache and dependencies are available. Logs and JSON stress results go
under ignored `build/`. Hardware tests require the correct prebuilt recovery bundle
and a physically connected compatible phone; host tests do not simulate a usable
installed iOS boot or guarantee future exploit success.

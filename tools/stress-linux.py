#!/usr/bin/env python3
"""Repeated host regressions; never opens a real USB device or alters a phone."""
import json
from pathlib import Path
import subprocess
import time

root = Path(__file__).resolve().parent.parent
build = root / 'build'
build.mkdir(exist_ok=True)
started = time.monotonic()
results = []
with (build / 'stress.log').open('w') as log:
    suites = [(['build/test-usb'], 100), (['build/test-pongo'], 100),
              (['python3', '-m', 'unittest', 'discover', '-s', 'tests'], 20),
              (['build/test-exploit'], 10)]
    for command, repetitions in suites:
        print(f'Running {repetitions} repetitions: {" ".join(command)}', flush=True)
        for iteration in range(repetitions):
            log.write(f'\n{command!r}, iteration {iteration + 1}\n')
            log.flush()
            subprocess.run(command, cwd=root, stdout=log, stderr=log, check=True, timeout=60)
        results.append({'command': command, 'passed_repetitions': repetitions})
(build / 'stress-results.json').write_text(json.dumps({
    'suites': results, 'seconds': round(time.monotonic() - started, 3),
    'hardware_test': False}, indent=2) + '\n')
print('All repeated regressions passed; details: build/stress-results.json', flush=True)

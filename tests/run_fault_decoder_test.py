"""Compare the native decoder against FocDriver's shared C safety engine."""
import argparse
import importlib.util
from pathlib import Path
import random
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("foc_driver", type=Path)
args = parser.parse_args()
subprocess.run([sys.executable, "scripts/sync_motor_faults.py", str(args.foc_driver), "--check"], cwd=root, check=True)
spec = importlib.util.spec_from_file_location("firmware_diagnostics", args.foc_driver / "tools/safety/safety_generate.py")
firmware = importlib.util.module_from_spec(spec)
sys.dont_write_bytecode = True
spec.loader.exec_module(firmware)
catalog = firmware.build_catalog(args.foc_driver)
spec = importlib.util.spec_from_file_location("safety_reference", args.foc_driver / "tools/safety/safety_decode.py")
reference = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = reference
spec.loader.exec_module(reference)
profile = reference.load_profile(args.foc_driver / "config/safety.json")
output = root / "build/fault-decoder-check"
output.mkdir(parents=True, exist_ok=True)
binary = output / "fault_decoder_test.exe"
subprocess.run([shutil.which("g++") or "g++", "-std=c++20", "-O2", "-Isrc", "tests/fault_decoder_test.cpp", "-o", str(binary)], cwd=root, check=True)
rng = random.Random(42)
requests, expected = [], []
for name, page in catalog["pages"].items():
    values = [0, 0xFFFFFFFF, 0x80000001, 3] + [1 << bit for bit in range(32)]
    values += [entry.get("value", 0) for entry in page["entries"]] + [rng.getrandbits(32) for _ in range(100)]
    for value in values:
        decoder = profile["_decoders"][firmware.SAFETY_PAGE_IDS.get(name, name)]
        result = reference.describe(decoder, reference.parse_value(value, "u32", 32))
        selected = {item["name"] for item in result["findings"]}
        requests.append(f"{name} {value}")
        expected.append([0 if page["mode"] == "predrv" else int(result.get("unknown_mask", "0"), 16),
                         value & 0x7FFFFFFF if page["mode"] == "predrv" else 0,
                         int(result["unknown"] if page["mode"] == "enum" else False),
                         *[entry["value" if page["mode"] == "enum" else "bit"] for entry in page["entries"] if entry["name"] in selected]])

result = subprocess.run([str(binary)], input="\n".join(requests), text=True, capture_output=True, check=True)
actual = [[int(part) for part in line.split()] for line in result.stdout.splitlines()]
assert len(actual) == len(expected)
for request, got, want in zip(requests, actual, expected):
    assert got == want, (request, got, want)
print(f"PASS: {len(requests)} differential cases across {len(catalog['pages'])} diagnostic pages; strict numeric parsing")

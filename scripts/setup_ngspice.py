"""Install the pinned official ngspice Windows runtime in this workspace."""
from pathlib import Path
import hashlib
import shutil
import subprocess
import urllib.request

root = Path(__file__).resolve().parents[1]
cache = root / "build/spice-download"
cache.mkdir(parents=True, exist_ok=True)
archive = cache / "ngspice-47_64.7z"
expected = "59225971bd68cdd1199443649aa4615a9e6d684933f205ab49006a3942518f5a"
if not archive.exists():
    urllib.request.urlretrieve(
        "https://downloads.sourceforge.net/project/ngspice/ng-spice-rework/47/ngspice-47_64.7z", archive)
if hashlib.sha256(archive.read_bytes()).hexdigest() != expected:
    raise SystemExit("ngspice archive SHA-256 differs from the official release")
seven_zip = shutil.which("7z") or shutil.which("7za")
if not seven_zip:
    raise SystemExit("7-Zip is required to extract the official package")
unpacked = cache / "runtime"
subprocess.run([seven_zip, "x", str(archive), "-o" + str(unpacked), "-y"], check=True,
               stdout=subprocess.DEVNULL)
runtime = root / "thirdparty/ngspice/Spice64"
shutil.copytree(unpacked / "Spice64", runtime, dirs_exist_ok=True)
shutil.copyfile(runtime / "docs/COPYING", runtime.parent / "LICENSES.txt")
if (root / "bin/win/ava_tool.exe").exists():
    shutil.copytree(runtime, root / "bin/win/Spice64", dirs_exist_ok=True)
print("Installed:", runtime)
subprocess.run([str(runtime / "bin/ngspice_con.exe"), "--version"], check=True)

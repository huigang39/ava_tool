"""Protocol target simulation + optional host JNI test of the production shared ELF/DWARF code."""
import argparse
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
def run(*args): subprocess.run([str(a) for a in args], cwd=ROOT, check=True, timeout=180)
def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--jdk', default=os.environ.get('JAVA_HOME'))
    p.add_argument('--native', action='store_true')
    args = p.parse_args()
    if not args.jdk: p.error('--jdk or JAVA_HOME required')
    jdk = Path(args.jdk); suffix = '.exe' if os.name == 'nt' else ''
    build = ROOT / 'build/android-tests'; build.mkdir(parents=True, exist_ok=True)
    source = ROOT / 'src/platform/android/java/com/ava/tool'
    java = jdk / ('bin/java' + suffix)
    run(jdk / ('bin/javac' + suffix), '-encoding', 'UTF-8', '-d', build,
        source / 'Jlink.java', source / 'JlinkHss.java', source / 'HssCapture.java', source / 'Values.java', source / 'NativeCore.java', source / 'DeviceDiagnostics.java', source / 'AndroidUsbTransport.java',
        *sorted((ROOT / 'tests/android/usb_stubs').rglob('*.java')),
        ROOT / 'tests/android/ProtocolTest.java', ROOT / 'tests/android/NativeCoreTest.java',
        ROOT / 'tests/android/DiagnosticsTest.java', ROOT / 'tests/android/UsbTransportTest.java', ROOT / 'tests/android/HssTest.java')
    run(java, '-cp', build, 'com.ava.tool.ProtocolTest')
    run(java, '-cp', build, 'com.ava.tool.DiagnosticsTest')
    run(java, '-cp', build, 'com.ava.tool.UsbTransportTest')
    run(java, '-cp', build, 'com.ava.tool.HssTest')
    if not args.native: return
    gcc = shutil.which('arm-none-eabi-gcc')
    if not gcc: p.error('--native requires arm-none-eabi-gcc to build the DWARF fixture')
    fixture = build / 'fixture.elf'
    run(gcc, '-g', '-gdwarf-4', '-O0', '-nostdlib', '-Wl,-e,main', '-Wl,-Ttext=0x08000000',
        '-Wl,-Tdata=0x20000000', ROOT / 'tests/android/symbol_fixture.c', '-o', fixture)
    native = build / 'native'
    opts = ['-G', 'Visual Studio 17 2022', '-A', 'x64'] if os.name == 'nt' else []
    run('cmake', '-S', ROOT / 'src/platform/android', '-B', native, *opts, f'-DJAVA_HOME={jdk.as_posix()}')
    run('cmake', '--build', native, '--config', 'Release', '--parallel', '4')
    malformed = []
    data = fixture.read_bytes()
    for name, content in [('truncated', data[:30]), ('wrong_machine', data[:18] + b'\x3e\x00' + data[20:]),
                           ('bad_sections', data[:32] + struct.pack('<I', 0xfffffff0) + data[36:])]:
        path = build / (name + '.elf'); path.write_bytes(content); malformed.append(path)
    dll_dir = native / 'Release' if os.name == 'nt' else native
    run(java, f'-Djava.library.path={dll_dir}', '-cp', build, 'com.ava.tool.NativeCoreTest', fixture, *malformed)
if __name__ == '__main__': main()

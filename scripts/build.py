"""Unified build entry for win, linux, mac and Android. No downloaded dependencies are installed globally."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]

def run(*args, cwd=ROOT):
    command = [str(a) for a in args]
    print('+', subprocess.list2cmdline(command), flush=True)
    subprocess.run(command, cwd=cwd, check=True)

def android(args):
    sdk_value = args.sdk or os.environ.get('ANDROID_SDK_ROOT') or os.environ.get('ANDROID_HOME')
    ndk_value = args.ndk or os.environ.get('ANDROID_NDK_HOME')
    jdk_value = args.jdk or os.environ.get('JAVA_HOME')
    if not sdk_value or not ndk_value or not jdk_value:
        raise ValueError('Android requires --sdk, --ndk, --jdk (or ANDROID_SDK_ROOT, ANDROID_NDK_HOME, JAVA_HOME)')
    sdk, ndk, jdk = (Path(p).resolve() for p in (sdk_value, ndk_value, jdk_value))
    suffix = '.exe' if os.name == 'nt' else ''
    tools = sdk / 'build-tools' / args.build_tools
    jar = sdk / 'platforms/android-35/android.jar'
    source = ROOT / 'src/platform/android'
    build = ROOT / 'build/android'
    output = ROOT / 'bin/android'
    for folder in (build, output, build / 'classes', build / 'gen', build / 'dex'):
        folder.mkdir(parents=True, exist_ok=True)
    for path in (jar, tools / ('aapt2' + suffix), jdk / ('bin/javac' + suffix), ndk / 'build/cmake/android.toolchain.cmake'):
        if not path.is_file(): raise ValueError(f'Missing tool: {path}')
    cmake = args.cmake or shutil.which('cmake')
    ninja = args.ninja or shutil.which('ninja')
    if not cmake or not ninja: raise ValueError('Install CMake and Ninja, or pass --cmake / --ninja')
    java = jdk / ('bin/java' + suffix)
    run(cmake, '-S', source, '-B', build / 'native', '-G', 'Ninja',
        f'-DCMAKE_MAKE_PROGRAM={ninja}', f'-DCMAKE_TOOLCHAIN_FILE={ndk}/build/cmake/android.toolchain.cmake',
        '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-26', '-DANDROID_STL=c++_static', '-DCMAKE_BUILD_TYPE=Release', '-Wno-dev')
    run(cmake, '--build', build / 'native', '--parallel', args.jobs)
    run(tools / ('aapt2' + suffix), 'compile', '--dir', source / 'res', '-o', build / 'resources.zip')
    run(tools / ('aapt2' + suffix), 'link', '-o', build / 'base.apk', '-I', jar,
        '--manifest', source / 'AndroidManifest.xml', '--java', build / 'gen', build / 'resources.zip')
    # Recreate only generated class files to avoid packaging stale classes after source removals.
    for path in (build / 'classes').rglob('*.class'): path.unlink()
    sources = sorted((source / 'java').rglob('*.java')) + sorted((build / 'gen').rglob('*.java'))
    run(jdk / ('bin/javac' + suffix), '-encoding', 'UTF-8', '-source', '8', '-target', '8', '-classpath', jar,
        '-d', build / 'classes', *sources)
    with zipfile.ZipFile(build / 'classes.jar', 'w', zipfile.ZIP_DEFLATED) as archive:
        for path in (build / 'classes').rglob('*.class'): archive.write(path, path.relative_to(build / 'classes').as_posix())
    run(java, '-cp', tools / 'lib/d8.jar', 'com.android.tools.r8.D8', '--min-api', '26', '--lib', jar,
        '--output', build / 'dex', build / 'classes.jar')
    shutil.copy2(build / 'base.apk', build / 'unsigned.apk')
    with zipfile.ZipFile(build / 'unsigned.apk', 'a', zipfile.ZIP_DEFLATED) as archive:
        archive.write(build / 'dex/classes.dex', 'classes.dex')
        archive.write(build / 'native/libava_core.so', 'lib/arm64-v8a/libava_core.so')
        cjson = (ROOT / 'thirdparty/cJSON/cJSON.c').read_text(encoding='utf-8')
        archive.writestr('assets/licenses/cJSON.txt', cjson.split('*/', 1)[0].removeprefix('/*'))
        for notice in ('NOTICE', 'NOTICE.toolchain'):
            if (ndk / notice).is_file(): archive.write(ndk / notice, 'assets/licenses/NDK-' + notice + '.txt')
    run(tools / ('zipalign' + suffix), '-f', '-P', '16', '4', build / 'unsigned.apk', build / 'aligned.apk')
    key = ROOT / '.local-signing/android-debug.keystore'
    if not key.is_file():
        key.parent.mkdir(exist_ok=True)
        run(jdk / ('bin/keytool' + suffix), '-genkeypair', '-keystore', key, '-storepass', 'android', '-keypass', 'android',
            '-alias', 'androiddebugkey', '-keyalg', 'RSA', '-keysize', '2048', '-validity', '10000', '-dname', 'CN=AvA Android Development')
    apk = output / 'ava_tool.apk'
    run(java, '-jar', tools / 'lib/apksigner.jar', 'sign', '--ks', key, '--ks-key-alias', 'androiddebugkey',
        '--ks-pass', 'pass:android', '--key-pass', 'pass:android', '--out', apk, build / 'aligned.apk')
    run(java, '-jar', tools / 'lib/apksigner.jar', 'verify', '--verbose', apk)
    run(tools / ('zipalign' + suffix), '-c', '-P', '16', '4', apk)
    print(f'APK: {apk}')

def main():
    host = 'win' if sys.platform == 'win32' else 'mac' if sys.platform == 'darwin' else 'linux'
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('platform', nargs='?', default=host, choices=('win', 'linux', 'mac', 'android'))
    p.add_argument('--jobs', type=int, default=4)
    p.add_argument('--sdk'); p.add_argument('--ndk'); p.add_argument('--jdk')
    p.add_argument('--build-tools', default='35.0.0')
    p.add_argument('--cmake'); p.add_argument('--ninja')
    args = p.parse_args()
    if args.jobs < 1: p.error('--jobs must be positive')
    if args.platform == 'android': android(args)
    else:
        if args.platform != host: p.error(f'Build {args.platform} desktop on its native OS; current host is {host}')
        make = shutil.which('make')
        if not make: p.error('GNU make is required')
        run(make, f'-j{args.jobs}', 'all', f'PLATFORM={args.platform}')

if __name__ == '__main__':
    try: main()
    except (ValueError, subprocess.CalledProcessError) as error:
        print(f'Build failed: {error}', file=sys.stderr)
        sys.exit(1)

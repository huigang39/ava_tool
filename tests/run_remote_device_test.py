"""Build and run the portable peer core against fake RAM; no physical hardware."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx',default='g++')
    parser.add_argument('--jdk',help='Also build/run host JNI callbacks with this JDK')
    args=parser.parse_args();out=ROOT/'build/remote-device-tests';out.mkdir(parents=True,exist_ok=True)
    def run(*cmd):subprocess.run([str(c) for c in cmd],cwd=ROOT,check=True,timeout=180)
    # .exe suffix is also valid on POSIX and keeps the wire fixture path stable.
    binary=out/'remote_device_test.exe'
    libs=['-lws2_32','-lbcrypt'] if os.name=='nt' else ['-pthread']
    run(args.cxx,'-std=c++20','-O0','-g','-Isrc','-Ithirdparty/cJSON','src/core/remote_device.cpp','thirdparty/cJSON/cJSON.c','tests/remote_device_test.cpp',*libs,'-o',binary)
    run(binary);run(sys.executable,'tests/remote_device_wire_test.py')
    if args.jdk:
        jdk=Path(args.jdk);suffix='.exe' if os.name=='nt' else ''
        opts=['-G','Visual Studio 17 2022','-A','x64','-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded'] if os.name=='nt' else []
        run('cmake','-S','src/platform/android','-B',out/'jni',*opts,'-DJAVA_HOME='+jdk.as_posix())
        run('cmake','--build',out/'jni','--config','Release','--parallel','4')
        run(jdk/('bin/javac'+suffix),'-encoding','UTF-8','-d',out/'java','src/platform/android/java/com/ava/tool/NativePeer.java','tests/android/PeerJniTest.java')
        native=out/'jni/Release' if os.name=='nt' else out/'jni'
        run(jdk/('bin/java'+suffix),'-Djava.library.path='+str(native),'-cp',out/'java','com.ava.tool.PeerJniTest')

if __name__=='__main__':main()

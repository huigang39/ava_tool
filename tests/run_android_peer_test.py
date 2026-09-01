"""Bidirectional desktop/Android JNI interoperability on an isolated emulator only."""
import argparse
import os
from pathlib import Path
import subprocess
import zipfile
from remote_device_wire_test import fixture, verify

ROOT=Path(__file__).resolve().parents[1]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sdk',required=True);p.add_argument('--jdk',required=True)
    p.add_argument('--serial',required=True);p.add_argument('--port',default='5039')
    a=p.parse_args()
    if not a.serial.startswith('emulator-'):p.error('This test never targets a physical device')
    sdk=Path(a.sdk).resolve();jdk=Path(a.jdk).resolve();build=ROOT/'build/android-peer-tests'
    classes=build/'classes';dex=build/'dex'
    classes.mkdir(parents=True,exist_ok=True);dex.mkdir(parents=True,exist_ok=True)
    suffix='.exe' if os.name=='nt' else '';bt=sdk/'build-tools/35.0.0';jar=sdk/'platforms/android-35/android.jar'
    java=jdk/('bin/java'+suffix)
    adb=[str(sdk/('platform-tools/adb'+suffix)),'-P',a.port,'-s',a.serial]
    def run(*args):subprocess.run([str(x) for x in args],check=True,cwd=ROOT,timeout=90)
    manifest=build/'AndroidManifest.xml'
    manifest.write_text('<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="com.ava.tool.peer_tests"><uses-sdk android:minSdkVersion="26" android:targetSdkVersion="35"/><application android:label="AvA peer tests"/><instrumentation android:name="com.ava.tool.PeerInstrumentation" android:targetPackage="com.ava.tool"/></manifest>')
    run(bt/('aapt2'+suffix),'link','-I',jar,'--manifest',manifest,'-o',build/'base.apk')
    run(jdk/('bin/javac'+suffix),'-encoding','UTF-8','-source','8','-target','8','-cp',str(jar)+os.pathsep+str(ROOT/'build/android/classes'),'-d',classes,ROOT/'tests/android/PeerInstrumentation.java')
    with zipfile.ZipFile(build/'tests.jar','w') as z:
        for path in classes.rglob('*.class'):z.write(path,path.relative_to(classes).as_posix())
    run(java,'-cp',bt/'lib/d8.jar','com.android.tools.r8.D8','--min-api','26','--lib',jar,'--classpath',ROOT/'build/android/classes.jar','--output',dex,build/'tests.jar')
    import shutil
    shutil.copy2(build/'base.apk',build/'unsigned.apk')
    with zipfile.ZipFile(build/'unsigned.apk','a') as z:z.write(dex/'classes.dex','classes.dex')
    run(bt/('zipalign'+suffix),'-f','4',build/'unsigned.apk',build/'aligned.apk')
    run(java,'-jar',bt/'lib/apksigner.jar','sign','--ks',ROOT/'.local-signing/android-debug.keystore','--ks-key-alias','androiddebugkey','--ks-pass','pass:android','--key-pass','pass:android','--out',build/'tests.apk',build/'aligned.apk')
    run(*adb,'install','-r',build/'tests.apk')
    run(*adb,'forward','tcp:45995','tcp:45994')
    process=fixture();instrument=None
    try:
        instrument=subprocess.Popen(adb+['shell','am','instrument','-w','com.ava.tool.peer_tests/com.ava.tool.PeerInstrumentation'],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
        lines=[]
        for line in instrument.stdout:
            lines.append(line)
            if 'phase=server-ready' in line:break
        else:raise AssertionError('Android peer failed: '+''.join(lines))
        verify('127.0.0.1',45995)
        output=instrument.communicate(timeout=25)[0];lines.append(output)
        (build/'result.log').write_text(''.join(lines),encoding='utf-8')
        assert 'PASS: Android controller and exporter' in output,output
        print(output,flush=True)
    finally:
        if instrument and instrument.poll() is None:instrument.kill();instrument.communicate()
        process.communicate('\n',timeout=15)
        run(*adb,'forward','--remove','tcp:45995')
        run(*adb,'uninstall','com.ava.tool.peer_tests')

if __name__=='__main__':main()

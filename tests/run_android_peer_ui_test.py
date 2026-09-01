"""Connect the Android UI to fake desktop RAM and exercise the existing scope."""
import argparse
from pathlib import Path
import re
import subprocess
import time
import xml.etree.ElementTree as ET
from remote_device_wire_test import fixture, KEY

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--adb',required=True);p.add_argument('--serial',required=True);p.add_argument('--port',default='5039')
    a=p.parse_args()
    if not a.serial.startswith('emulator-'):p.error('Emulator only')
    prefix=[a.adb,'-P',a.port,'-s',a.serial]
    out=Path('build/android-ui/peer-screenshots');out.mkdir(parents=True,exist_ok=True)
    def adb(*args):return subprocess.check_output(prefix+list(args),timeout=35)
    def sh(*args):return adb('shell',*args).decode('utf-8',errors='replace')
    def tree():
        sh('rm','-f','/sdcard/ava-peer-ui.xml')
        sh('uiautomator','dump','/sdcard/ava-peer-ui.xml')
        return ET.fromstring(sh('cat','/sdcard/ava-peer-ui.xml'))
    def find(value,key='text'):
        nodes=[n for n in tree().iter('node') if n.get(key)==value]
        assert nodes,value
        return nodes[-1]
    def xy(node):
        x1,y1,x2,y2=map(int,re.findall(r'\d+',node.get('bounds')))
        return str((x1+x2)//2),str((y1+y2)//2)
    def tap(value,key='text'):sh('input','tap',*xy(find(value,key)))
    def field(index,value):
        fields=[n for n in tree().iter('node') if n.get('class')=='android.widget.EditText']
        sh('input','tap',*xy(fields[index]));sh('input','keyevent','123')
        old=fields[index].get('text','')
        if old:sh('input','keyevent',*(['67']*len(old)))
        sh('input','text',value)
    def capture(name):
        (out/(name+'.xml')).write_bytes(ET.tostring(tree(),encoding='utf-8'))
        (out/(name+'.png')).write_bytes(adb('exec-out','screencap','-p'))
    process=fixture()
    try:
        sh('am','force-stop','com.ava.tool');sh('am','start','-W','-n','com.ava.tool/.MainActivity')
        tap('远程设备');tap('连接远端');field(0,'10.0.2.2');field(1,'45993');field(2,KEY)
        sh('input','keyevent','4');tap('连接');time.sleep(.3)
        find('远程设备 · 已连接');capture('01-connected')
        tap('变量页面','content-desc');tap('手动添加');field(0,'peer_ui_signal');field(1,'20000000');tap('添加')
        tap('peer_ui_signal');start=xy(find('开始采样'))
        sh('input','tap',*start);time.sleep(.6);sh('input','tap',*start)
        data=ET.tostring(tree(),encoding='unicode')
        assert '1.25' in data,data
        capture('02-remote-scope');print('PASS: Android UI paired with desktop, existing variable/scope shows timestamped remote samples',flush=True)
        tap('设备页面','content-desc');tap('申请 / 释放控制权');time.sleep(.2)
        assert find('复位远端 MCU…').get('enabled')=='true'
        tap('使用本机设备');find('J-Link 未连接');capture('03-local-disconnected')
        print('PASS: explicit control acquisition, switch back to disconnected local device',flush=True)
    finally:
        sh('am','force-stop','com.ava.tool');process.communicate('\n',timeout=15)

if __name__=='__main__':main()

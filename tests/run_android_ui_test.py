"""Exercise the installed touch UI on an isolated emulator, without USB hardware.

Requires the app already installed. Adds one local watch, switches appearance,
and temporarily changes the emulator's display size/font scale (restored at exit).
"""
import argparse
from pathlib import Path
import re
import subprocess
import xml.etree.ElementTree as ET


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--adb', required=True)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--port', default='5037')
    parser.add_argument('--output', default='build/android-ui/screenshots')
    args = parser.parse_args()
    if not args.serial.startswith('emulator-'):
        parser.error('Use an isolated emulator; this test never targets a phone')
    output = Path(args.output); output.mkdir(parents=True, exist_ok=True)
    prefix = [args.adb, '-P', args.port, '-s', args.serial]

    def adb(*command):
        return subprocess.check_output(prefix + list(command), timeout=40)

    def shell(*command):
        return adb('shell', *command).decode('utf-8', errors='replace')

    def tree():
        shell('uiautomator', 'dump', '/sdcard/ava-ui-test.xml')
        return ET.fromstring(shell('cat', '/sdcard/ava-ui-test.xml'))

    def find(key, value):
        matches = [n for n in tree().iter('node') if n.get(key) == value]
        assert matches, f'Missing {key}: {value}'
        return matches[-1]

    def bounds(node):
        return [int(v) for v in re.findall(r'\d+', node.get('bounds'))]

    def tap_node(node):
        x1, y1, x2, y2 = bounds(node)
        assert node.get('enabled') == 'true', 'Attempted to tap a disabled control'
        shell('input', 'tap', str((x1+x2)//2), str((y1+y2)//2))

    def tap(value, key='text'):
        tap_node(find(key, value))

    def capture(name):
        data = tree()
        for title in ('设备', '变量', '示波器', '设置'):
            nav = [n for n in data.iter('node') if n.get('content-desc') == title + '页面']
            assert len(nav) == 1, f'Navigation missing: {title}'
            x1, y1, x2, y2 = bounds(nav[0]); assert x2 > x1 and y2 > y1
        (output / (name + '.xml')).write_bytes(ET.tostring(data, encoding='utf-8'))
        (output / (name + '.png')).write_bytes(adb('exec-out', 'screencap', '-p'))
        print('PASS:', name, flush=True)

    original_font = shell('settings', 'get', 'system', 'font_scale').strip()
    original_size = shell('wm', 'size')
    original_override = re.search(r'Override size: (\d+x\d+)', original_size)
    try:
        shell('am', 'force-stop', 'com.ava.tool')
        shell('am', 'start', '-W', '-n', 'com.ava.tool/.MainActivity')
        capture('01-device')
        tap('变量页面', 'content-desc')
        assert find('text', '开始采样').get('enabled') == 'false', 'Sampling enabled without connection'
        tap('手动添加')
        fields = [n for n in tree().iter('node') if n.get('class') == 'android.widget.EditText']
        assert len(fields) == 2
        tap_node(fields[0]); shell('input', 'text', 'ui_test_signal')
        fields = [n for n in tree().iter('node') if n.get('class') == 'android.widget.EditText']
        tap_node(fields[1]); shell('input', 'text', '20000000')
        tap('添加')
        find('text', 'ui_test_signal')
        capture('02-variables')
        tap('ui_test_signal')
        find('text', '最小值'); find('text', '最大值'); find('text', '当前值')
        capture('03-scope')
        tap('ui_test_signal  ▾')
        find('text', '显示通道'); tap('取消')
        tap('设置页面', 'content-desc')
        find('text', '普通采样'); capture('04-settings-sampling')
        tap('外观')
        choice = next(n for n in tree().iter('node') if n.get('text', '').startswith('外观：'))
        tap_node(choice); tap('深色')
        tap('示波器页面', 'content-desc'); capture('05-dark-scope')
        tap('设置页面', 'content-desc'); tap('日志')
        assert '0.1.7-android' in ET.tostring(tree(), encoding='unicode')
        capture('06-settings-logs')
        shell('settings', 'put', 'system', 'font_scale', '1.4')
        shell('wm', 'size', '480x960')
        shell('am', 'start', '-W', '-n', 'com.ava.tool/.MainActivity')
        tap('示波器页面', 'content-desc'); capture('07-narrow-large-font')
        tap('变量页面', 'content-desc'); find('text', 'ui_test_signal')
        tap('设备页面', 'content-desc'); find('text', '连接 J-Link')
        print('PASS: navigation, add watch, scope selector, settings tabs, dark mode, narrow display, recreation; no hardware accessed', flush=True)
    finally:
        if original_font == 'null': shell('settings', 'delete', 'system', 'font_scale')
        else: shell('settings', 'put', 'system', 'font_scale', original_font)
        shell('wm', 'size', original_override.group(1) if original_override else 'reset')


if __name__ == '__main__':
    main()

"""Independent AVA peer protocol client and hostile-frame regression (fake RAM only)."""
import hashlib
import hmac
import json
from pathlib import Path
import socket
import struct
import subprocess
import time

KEY = '0123456789abcdef0123456789abcdef'
ROOT = Path(__file__).resolve().parents[1]

class Peer:
    def __init__(self, host, port, key=KEY):
        self.s = socket.create_connection((host, port), timeout=5)
        self.key = key.encode(); self.tx = self.rx = 0
        hello = json.loads(self.frame()); assert hello['protocol'] == 1
        self.nonce = hello['nonce']
        proof = self.mac('AVA_PEER/1:' + self.nonce)
        self.send_frame(json.dumps({'proof': proof}).encode())
        self.info = self.receive()
    def mac(self, text):
        return hmac.new(self.key, text.encode(), hashlib.sha256).hexdigest()
    def exact(self, size):
        result = bytearray()
        while len(result) < size:
            chunk = self.s.recv(size-len(result))
            if not chunk: raise EOFError('Peer closed')
            result.extend(chunk)
        return result
    def frame(self):
        n, = struct.unpack('!I', self.exact(4))
        assert 0 < n <= 192*1024
        return self.exact(n)
    def send_frame(self, payload, fragmented=False):
        data = struct.pack('!I', len(payload)) + payload
        if fragmented:
            for b in data: self.s.sendall(bytes([b]))
        else: self.s.sendall(data)
    def envelope(self, value):
        self.tx += 1; payload = json.dumps(value, separators=(',', ':'))
        return json.dumps({'seq':self.tx,'payload':payload,'mac':self.mac(self.nonce+'C'+str(self.tx)+':'+payload)}, separators=(',', ':')).encode()
    def receive(self):
        msg = json.loads(self.frame()); self.rx += 1
        assert msg['seq'] == self.rx
        assert hmac.compare_digest(msg['mac'],self.mac(self.nonce+'S'+str(self.rx)+':'+msg['payload']))
        return json.loads(msg['payload'])
    def call(self, op, **kwargs):
        value = {'op':op,**kwargs}; self.send_frame(self.envelope(value)); return self.receive()
    def close(self):
        self.s.close()

def fixture():
    process = subprocess.Popen([str(ROOT/'build/remote-device-tests/remote_device_test.exe'),'--serve'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    assert process.stdout.readline().startswith('READY')
    return process

def verify(host='127.0.0.1', port=45993):
    a=Peer(host,port); epoch=a.info['epoch']; assert a.info['sampling']=='timestamped-poll'
    envelope=a.envelope({'op':'info'});a.send_frame(envelope,fragmented=True);assert a.receive()['ok']
    assert not a.call('write',epoch=epoch,address=0x20000000,size=4,data='00006040')['ok']
    assert a.call('claim',acquire=True)['ok']
    assert a.call('write',epoch=epoch,address=0x20000000,size=4,data='00006040')['ok']
    assert a.call('read',epoch=epoch,address=0x20000000,size=4)['data']=='00006040'
    assert not a.call('write',epoch=epoch+1,address=0x20000000,size=4,data='00000000')['ok']
    assert not a.call('read',epoch=epoch,address=0xffffffff,size=4)['ok']
    assert not a.call('start',epoch=epoch,periodUs=1,blocks=[{'address':0,'size':4097}])['ok']
    assert not a.call('flash',epoch=epoch)['ok']
    b=Peer(host,port); assert not b.call('claim',acquire=True)['ok']
    assert a.call('start',epoch=epoch,periodUs=1,blocks=[{'address':0x20000000,'size':4}])['periodUs']==20000
    time.sleep(.13)
    data=bytes.fromhex(a.call('samples',capacity=4096)['data']);assert len(data)>=16 and len(data)%8==0
    stamps=[struct.unpack_from('<I',data,i)[0] for i in range(0,len(data),8)]
    assert all(((y-x)&0xffffffff)>=19000 for x,y in zip(stamps,stamps[1:]))
    for i in range(0,len(data),8):assert data[i+4:i+8]==bytes.fromhex('00006040')
    # A byte-for-byte replay of an authenticated frame closes the session,
    # instead of re-running its mutation. A different peer can then take control.
    a.send_frame(envelope)
    try:a.receive();raise AssertionError('Replay accepted')
    except (EOFError,ConnectionResetError):pass
    a.close()
    for _ in range(30):
        if b.call('claim',acquire=True)['ok']:break
        time.sleep(.02)
    else:raise AssertionError('Lease not released after replay disconnect')
    assert b.call('read',epoch=epoch,address=0x20000000,size=4)['data']=='00006040'
    b.close()
    # Tampered MAC is rejected before dispatch.
    bad=Peer(host,port);msg=json.loads(bad.envelope({'op':'write','epoch':epoch,'address':0x20000000,'size':4,'data':'00000000'}));msg['mac']='0'*64;bad.send_frame(json.dumps(msg).encode())
    try:bad.receive();raise AssertionError('Invalid MAC accepted')
    except (EOFError,ConnectionResetError):pass
    bad.close()
    malformed=Peer(host,port)
    malformed.s.sendall(struct.pack('!I',0x7fffffff))
    try:malformed.receive();raise AssertionError('Oversized frame accepted')
    except (EOFError,ConnectionResetError):pass
    malformed.close()
    print('PASS: independent SHA-256/HMAC interoperability, split TCP frames, stale epochs, malformed ranges, capabilities, writer contention, source timestamps, replay and MAC rejection',flush=True)

if __name__=='__main__':
    process=fixture()
    try:verify()
    finally:process.communicate('\n',timeout=15)

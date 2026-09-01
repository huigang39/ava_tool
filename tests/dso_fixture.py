"""Loopback-only SCPI instrument for GUI integration tests; no hardware access."""
import math
import re
import socketserver
import threading
import time

class Instrument(socketserver.StreamRequestHandler):
    def handle(self):
        channel, points = 1, 1000
        malformed = False
        scale, offset, probe, coupling, enabled = [1.0]*4, [0.0]*4, [10.0]*4, ['DC']*4, [1]*4
        time_scale, time_position = 1e-8, 0.0
        error = '0,"No error"'
        while True:
            line = self.rfile.readline()
            if not line:
                return
            command = line.decode().strip().upper()
            response = None
            if command == '*IDN?':
                response = 'KEYSIGHT,DSOX2024A,TEST_ONLY,1.0'
            elif command == 'TEST:TIMEOUT?':
                time.sleep(.5)
                continue
            elif command == 'TEST:MALFORMED': malformed = True
            elif command == ':SYSTEM:ERROR?':
                response, error = error, '0,"No error"'
            elif command == 'INVALID':
                error = '-113,"Undefined header"'
            elif command.startswith(':CHANNEL'):
                m = re.fullmatch(r':CHANNEL([1-4]):(\w+)(\?| .+)', command)
                if not m:
                    raise AssertionError(command)
                i, field, value = int(m[1])-1, m[2], m[3]
                values = {'SCALE':scale, 'OFFSET':offset, 'PROBE':probe, 'COUPLING':coupling, 'DISPLAY':enabled}[field]
                if value == '?':
                    response = str(values[i])
                else:
                    value = value.strip()
                    values[i] = (1 if value == 'ON' else 0) if field == 'DISPLAY' else value if field == 'COUPLING' else float(value)
            elif command == ':TIMEBASE:SCALE?': response = str(time_scale)
            elif command == ':TIMEBASE:POSITION?': response = str(time_position)
            elif command.startswith(':TIMEBASE:SCALE '): time_scale = float(command.split()[-1])
            elif command.startswith(':TIMEBASE:POSITION '): time_position = float(command.split()[-1])
            elif command.startswith(':WAVEFORM:SOURCE '):
                channel = int(re.search(r'CHANNEL([1-4])', command)[1])
                points = max(32, min(1024, int(command.split()[-1])))
            elif command == ':WAVEFORM:PREAMBLE?': response = f'0,0,{"nan" if malformed else points},1,1e-9,{-points/2*1e-9},2,0.02,0,128'
            elif command == ':WAVEFORM:DATA?':
                data = bytes(round(128 + 60*math.sin(i*math.tau/80 + channel)) for i in range(points))
                length = str(len(data)).encode()
                packet = b'#' + str(len(length)).encode() + length + data + b'\n'
                self.wfile.write(packet[:5]); self.wfile.flush()
                self.wfile.write(packet[5:]); self.wfile.flush()
            elif command.startswith(':MEASURE:FREQUENCY?'): response = '12500000'
            elif command.startswith(':MEASURE:VPP?'): response = '2.4'
            elif command.startswith(':MEASURE:VRMS?'): response = '.85'
            elif command.startswith((':TRIGGER:', ':DIGITIZE ')) or command in (':RUN', ':STOP', ':SINGLE', ':AUTOSCALE', '*RST;*CLS'):
                pass
            else:
                raise AssertionError('Unexpected SCPI: ' + command)
            if response is not None:
                self.wfile.write((response + '\n').encode()); self.wfile.flush()

class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

def start():
    server = Server(('127.0.0.1', 0), Instrument)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server

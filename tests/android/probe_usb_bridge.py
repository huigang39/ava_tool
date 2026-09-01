"""Hardware lab only: use the installed Windows J-Link driver as byte transport.

The Java client owns the SWD/HSS protocol. This helper never calls SDK Connect,
HSS, reset, halt, flash, or memory APIs. Not shipped in the Android APK.
"""
import ctypes as C
import struct
import sys


def main():
    dll = C.CDLL(sys.argv[1])
    def bind(name, restype, *args):
        fn = getattr(dll, name)
        fn.restype, fn.argtypes = restype, args
        return fn
    command = bind('JLINKARM_ExecCommand', C.c_int, C.c_char_p, C.c_void_p, C.c_int)
    select = bind('JLINKARM_EMU_SelectByUSBSN', C.c_int, C.c_uint32)
    open_probe = bind('JLINKARM_Open', C.c_char_p)
    close = bind('JLINKARM_Close', None)
    lock = bind('JLINKARM_Lock', None)
    unlock = bind('JLINKARM_Unlock', None)
    write = bind('JLINKARM_WriteEmu', C.c_int, C.c_void_p, C.c_uint32)
    read = bind('JLINKARM_ReadEmu', C.c_int, C.c_void_p, C.c_uint32)
    output = sys.stdout.buffer
    source = sys.stdin.buffer
    def reply(data):
        output.write(struct.pack('>i', len(data)) + data)
        output.flush()
    locked = False
    try:
        ack = C.create_string_buffer(256)
        for text in (b'DisableAutoUpdateFW', b'SuppressGUI = 1'):
            if command(text, ack, len(ack)) < 0:
                raise RuntimeError(ack.value)
        if select(int(sys.argv[2])) < 0:
            raise RuntimeError('Probe selection failed')
        error = open_probe()
        if error:
            raise RuntimeError(error)
        reply(b'')
        while True:
            op = source.read(1)
            if not op or op == b'Q':
                break
            header = source.read(4)
            if len(header) != 4:
                raise RuntimeError('Truncated host request')
            n = struct.unpack('>I', header)[0]
            if not 0 < n <= 1048576:
                raise RuntimeError('Invalid transfer length')
            if op == b'W':
                data = source.read(n)
                if len(data) != n:
                    raise RuntimeError('Truncated host payload')
                buf = C.create_string_buffer(data)
                # SDK housekeeping runs on another thread. Keep it out between
                # command / length header / payload, not just each individual API.
                # Release only once the Java client begins the next transaction.
                if locked:
                    unlock()
                    locked = False
                lock()
                locked = True
                result = write(buf, n)
                if result != n:
                    raise RuntimeError(f'WriteEmu returned {result}, expected {n}')
                reply(b'')
            elif op == b'R':
                buf = C.create_string_buffer(n)
                result = read(buf, n)
                if result != n:
                    raise RuntimeError(f'ReadEmu returned {result}, expected {n}')
                reply(buf.raw)
            else:
                raise RuntimeError('Unknown host operation')
    except Exception as exc:
        message = str(exc).encode('utf-8')
        output.write(struct.pack('>i', -len(message)) + message)
        output.flush()
        raise
    finally:
        if locked:
            unlock()
        close()


if __name__ == '__main__':
    main()

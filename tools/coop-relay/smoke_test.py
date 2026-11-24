"""Smoke test for relay.py: host, two joiners, forwarding, broadcast, leave.
Usage: python smoke_test.py [host port]   (no args: starts a local relay)"""
import socket, struct, subprocess, sys, time

def tx(s, k, p=b''):
    d = bytes([k]) + p
    s.sendall(struct.pack('<I', len(d)) + d)

def rx(s):
    def exact(n):
        b = b''
        while len(b) < n:
            c = s.recv(n - len(b))
            if not c: raise EOFError
            b += c
        return b
    n = struct.unpack('<I', exact(4))[0]
    d = exact(n)
    return d[0], d[1:]

proc = None
if len(sys.argv) == 3:
    addr = (sys.argv[1], int(sys.argv[2]))
else:
    proc = subprocess.Popen([sys.executable, 'relay.py', '--port', '27961'])
    addr = ('127.0.0.1', 27961)
    time.sleep(1)
try:
    h = socket.create_connection(addr, timeout=5); tx(h, 1)
    k, code = rx(h); assert k == 2 and len(code) == 6, (k, code)
    a = socket.create_connection(addr, timeout=5); tx(a, 3, code.lower())
    assert rx(a) == (4, b'\x01'); assert rx(h) == (8, b'\x01')
    b = socket.create_connection(addr, timeout=5); tx(b, 3, code)
    assert rx(b) == (4, b'\x02'); assert rx(h) == (8, b'\x02')
    tx(a, 6, b'\x00hello host'); assert rx(h) == (7, b'\x01hello host')
    tx(h, 6, b'\x02to seat 2'); assert rx(b) == (7, b'\x00to seat 2')
    tx(h, 6, b'\xffall'); assert rx(a) == (7, b'\x00all') and rx(b) == (7, b'\x00all')
    big = bytes(3_000_000); tx(h, 6, b'\x01' + big); assert rx(a) == (7, b'\x00' + big)
    x = socket.create_connection(addr, timeout=5); tx(x, 3, b'NOPE00'); assert rx(x)[0] == 5
    a.close(); assert rx(h) == (9, b'\x01')
    print('relay OK, code', code.decode())
finally:
    if proc: proc.terminate(); proc.wait()

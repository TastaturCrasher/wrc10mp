"""Sends a RakNet unconnected ping to a WRC 10 lobby host and prints the reply.

usage: raknet_ping.py [host] [port]   (default 127.0.0.1 60000)
"""
import os, socket, struct, sys, time

MAGIC = bytes.fromhex('00ffff00fefefefefdfdfdfd12345678')  # RakNet OFFLINE_MESSAGE_DATA_ID
host = sys.argv[1] if len(sys.argv) > 1 else '127.0.0.1'
port = int(sys.argv[2]) if len(sys.argv) > 2 else 60000

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
s.settimeout(2)
for msg_id in (0x01, 0x02):  # ID_UNCONNECTED_PING, ID_UNCONNECTED_PING_OPEN_CONNECTIONS
    ping = bytes([msg_id]) + struct.pack('>Q', int(time.time() * 1000) & 0xFFFFFFFFFFFFFFFF) + MAGIC + os.urandom(8)
    s.sendto(ping, (host, port))
    try:
        while True:
            data, addr = s.recvfrom(4096)
            print(f'ping 0x{msg_id:02x}: {len(data)} bytes from {addr}, id 0x{data[0]:02x}')
            if data[0] == 0x1C and len(data) >= 33:  # ID_UNCONNECTED_PONG
                guid = data[9:17].hex()
                payload = data[33:]
                print(f'  guid {guid}, payload {len(payload)} bytes')
                print('  hex :', payload[:256].hex(' '))
                print('  text:', ''.join(chr(c) if 32 <= c < 127 else '.' for c in payload[:256]))
            else:
                print('  raw :', data[:96].hex(' '))
    except socket.timeout:
        pass

"""Pretends to be a WRC 10 LAN lobby host on UDP 60000 (discovery only).

Logs every RakNet unconnected ping (sender and destination type) and answers
with an unconnected pong carrying lobby data, so the game's lobby search
should list it. Connecting to it will fail - this only tests discovery.

usage: fake_host.py [port] [seconds] [--reply-from IP] [--to-loopback]

  --reply-from IP  send the pong from IP (any free port) instead of the
                   listening socket, e.g. 127.0.0.2 or the PC's LAN IP
  --to-loopback    deliver the pong to 127.0.0.1:<game port> instead of the
                   address the ping came from
"""
import argparse, os, socket, time

MAGIC = bytes.fromhex('00ffff00fefefefefdfdfdfd12345678')
# Captured from a real lobby: game id, password field, lobby parameters.
LOBBY_DATA = b'WRC10KyloKyloPwd;1 2;2 5;3 0;4 -1;0 0;5 0'

parser = argparse.ArgumentParser()
parser.add_argument('port', nargs='?', type=int, default=60000)
parser.add_argument('seconds', nargs='?', type=float, default=120)
parser.add_argument('--reply-from')
parser.add_argument('--to-loopback', action='store_true')
args = parser.parse_args()

guid = os.urandom(8)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(('0.0.0.0', args.port))
s.settimeout(0.2)
reply = s
if args.reply_from:
    reply = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    reply.bind((args.reply_from, 0))
    reply.settimeout(0.2)
print(f'listening on UDP {args.port} for {args.seconds:.0f}s, replying from {reply.getsockname()[0]}:{reply.getsockname()[1]}', flush=True)

end = time.time() + args.seconds
while time.time() < end:
    for sock in {s, reply}:
        try:
            data, addr = sock.recvfrom(4096)
        except (socket.timeout, ConnectionResetError):
            continue
        kind = {0x01: 'UNCONNECTED_PING', 0x02: 'UNCONNECTED_PING_OPEN'}.get(data[0], f'id 0x{data[0]:02x}')
        where = 'reply socket' if sock is not s else 'listen socket'
        print(f'{time.strftime("%H:%M:%S")} {kind} from {addr[0]}:{addr[1]} ({len(data)} bytes, {where})', flush=True)
        if data[0] in (0x01, 0x02) and len(data) >= 25 and data[9:25] == MAGIC:
            pong = b'\x1c' + data[1:9] + guid + MAGIC + LOBBY_DATA
            dest = ('127.0.0.1', addr[1]) if args.to_loopback else addr
            try:
                reply.sendto(pong, dest)
            except OSError as e:
                print(f'   pong to {dest} failed: {e}', flush=True)

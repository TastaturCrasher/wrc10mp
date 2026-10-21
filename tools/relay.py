"""wrc10mp relay - lets WRC 10's LAN mode reach a host over the internet.

Run this on the PC of the player who JOINS (not on the host). It listens on
the local LAN-lobby port (UDP 60000), catches the game's search broadcast and
all following game traffic, and forwards everything by unicast to the host
(e.g. its Tailscale IP). Replies from the host are sent back to the game, so
the host's lobby appears to be on the local network.

The relay pings the host itself in the background and answers the game's
search pings immediately from that cache. The answer is sent from a loopback
address (127.0.0.2 by default) to the game's 127.0.0.1 port, because the game
ignores lobbies that answer from one of the PC's own network addresses. The
game then connects to that loopback address, where the relay forwards the
connection to the host.

usage: relay.py <host-ip> [--port 60000] [--host-port 60000] [--game-ip 127.0.0.2] [--verbose]
"""
import argparse, os, select, socket, struct, time

MAGIC = bytes.fromhex('00ffff00fefefefefdfdfdfd12345678')  # RakNet OFFLINE_MESSAGE_DATA_ID
ID_UNCONNECTED_PING = 0x01
ID_UNCONNECTED_PING_OPEN = 0x02
ID_UNCONNECTED_PONG = 0x1C

parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
parser.add_argument('host', help='IP of the player hosting the lobby (e.g. Tailscale 100.x.y.z)')
parser.add_argument('--port', type=int, default=60000, help='local port the game searches on')
parser.add_argument('--host-port', type=int, default=60000, help='lobby port on the host')
parser.add_argument('--idle', type=float, default=120, help='drop a game client after N seconds of silence')
parser.add_argument('--probe', type=float, default=1.0, help='seconds between background pings to the host')
parser.add_argument('--game-ip', default='127.0.0.2', help='loopback address the game sees as the lobby host')
parser.add_argument('--verbose', action='store_true', help='log every packet')
args = parser.parse_args()

upstream_addr = (args.host, args.host_port)
# `listen` only catches the game's search broadcast. The game talks to the
# lobby through `local`, which it knows as <game-ip>:<port of local>.
listen = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
listen.bind(('0.0.0.0', args.port))
local = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
local.bind((args.game_ip, 0))
probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
probe.bind(('0.0.0.0', 0))
probe_guid = os.urandom(8)

# One upstream socket per game client address, so several local game
# instances (or retries from new ports) stay separate.
sessions = {}  # client addr -> [upstream socket, last activity]
by_socket = {}  # upstream socket -> client addr

lobby = None  # (host guid, lobby data) from the latest pong of the host
lobby_seen = 0.0
probe_sent = {}  # ping timestamp -> send time, for round-trip measurement
next_probe = 0.0


def log(msg):
    t = time.time()
    print(time.strftime('%H:%M:%S', time.localtime(t)) + f'.{int(t * 1000) % 1000:03d}', msg, flush=True)


def text(data):
    return ''.join(chr(c) if 32 <= c < 127 else '.' for c in data)


def is_ping(data):
    return len(data) >= 25 and data[0] in (ID_UNCONNECTED_PING, ID_UNCONNECTED_PING_OPEN) and data[9:25] == MAGIC


print(f'wrc10mp relay: search on UDP {args.port}, lobby as {local.getsockname()[0]}:{local.getsockname()[1]} '
      f'<-> {upstream_addr[0]}:{upstream_addr[1]}', flush=True)
print('Waiting for the host lobby, then start the lobby search in WRC 10. Ctrl+C to stop.', flush=True)

while True:
    now = time.time()
    if now >= next_probe:
        stamp = int(now * 1000)
        probe_sent[stamp] = now
        probe.sendto(bytes([ID_UNCONNECTED_PING_OPEN]) + struct.pack('>Q', stamp) + MAGIC + probe_guid, upstream_addr)
        next_probe = now + args.probe
        for k in [k for k, v in probe_sent.items() if now - v > 10]:
            del probe_sent[k]
    if lobby and now - lobby_seen > 5:
        log('!!! host lobby stopped answering')
        lobby = None

    readable, _, _ = select.select([listen, local, probe, *by_socket], [], [], 0.2)
    now = time.time()
    for sock in readable:
        try:
            data, addr = sock.recvfrom(65535)
        except ConnectionResetError:
            # ICMP port unreachable from a previous send: nothing listens there.
            side = {listen: 'game', local: 'game', probe: 'host (probe)'}.get(sock, 'host')
            if side != 'host (probe)' or lobby:
                log(f'!!! {side} side reported "port unreachable" (is the lobby still open?)')
            continue

        if sock is probe:
            if data[:1] == bytes([ID_UNCONNECTED_PONG]) and len(data) >= 33:
                stamp = struct.unpack('>Q', data[1:9])[0]
                rtt = (now - probe_sent.pop(stamp, now)) * 1000
                new = (data[9:17], data[33:])
                if new != lobby:
                    log(f'host lobby found, round trip {rtt:.0f} ms: {text(new[1])}')
                lobby, lobby_seen = new, now
            continue

        if sock in (listen, local) and is_ping(data):
            # Answer at once from the cache, echoing the game's timestamp, from the
            # loopback lobby address to the game's loopback port.
            dest = ('127.0.0.1', addr[1])
            if lobby:
                local.sendto(bytes([ID_UNCONNECTED_PONG]) + data[1:9] + lobby[0] + MAGIC + lobby[1], dest)
                log(f'search ping from {addr[0]}:{addr[1]} answered with host lobby -> {dest[0]}:{dest[1]}')
            else:
                log(f'search ping from {addr[0]}:{addr[1]} - no host lobby known yet')
            continue

        if sock is listen:
            if args.verbose:
                log(f'ignored id 0x{data[0]:02x} on search port from {addr[0]}:{addr[1]}')
            continue

        if sock is local:
            session = sessions.get(addr)
            if not session:
                up = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                up.bind(('0.0.0.0', 0))
                session = sessions[addr] = [up, now]
                by_socket[up] = addr
                log(f'new game client {addr[0]}:{addr[1]}')
            session[1] = now
            session[0].sendto(data, upstream_addr)
            if args.verbose:
                log(f'game -> host  id 0x{data[0]:02x} {len(data)} bytes')
        else:
            client = by_socket[sock]
            sessions[client][1] = now
            local.sendto(data, client)
            if args.verbose:
                log(f'host -> game  id 0x{data[0]:02x} {len(data)} bytes')

    for addr, (up, last) in list(sessions.items()):
        if now - last > args.idle:
            log(f'game client {addr[0]}:{addr[1]} idle, closed')
            del by_socket[up], sessions[addr]
            up.close()

#!/usr/bin/env python3
'''
PROJECT:     LiberNT Network Kernel Debugger Transport
LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
PURPOSE:     Rendezvous relay: forwards the encrypted datagrams between targets and debugger clients
COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
'''

import argparse
import hashlib
import hmac
import json
import os
import signal
import socket
import struct
import sys
import time

MAGIC = b'LKDN'
VERSION = 1
TYPE_HELLO, TYPE_INIT, TYPE_RESPONSE, TYPE_DATA, TYPE_SUBSCRIBE = 1, 2, 3, 4, 5
HEADER_SIZE = 20
HELLO_SIZE = 72
MAXIMUM_DATAGRAM = 1500
DEVICE_TIMEOUT = 30.0
CLIENT_TIMEOUT = 60.0
MAXIMUM_DEVICES = 4096
MAXIMUM_CLIENTS = 4
COOKIE_SIZE = 16
COOKIE_PERIOD = 64.0
RATE_PACKETS = 400
RATE_WINDOW = 1.0
RECORD_DAYS = 30
RECORD_PERIOD = 60.0
RECORD_FILE_LIMIT = 16 * 1024 * 1024
RECORD_DAY_LIMIT = 512 * 1024 * 1024
RECORD_MACHINES = 1024
RECORD_COUNTERS = 4096
RECORD_PRUNE = 3600.0
UNKNOWN = 'unknown'


class Recorder:
    def __init__(self, root):
        self.root = root
        self.day = None
        self.spent = 0
        self.machines = set()
        self.counters = {}
        self.flushed = time.monotonic()
        self.pruned = None

    def usage(self, day):
        names, spent = set(), 0
        for name in os.listdir(self.root) if os.path.isdir(self.root) else []:
            path = os.path.join(self.root, name, day + '.jsonl')
            if os.path.isfile(path):
                names.add(name)
                spent += os.path.getsize(path)
        return names, spent

    def write(self, name, record):
        if not self.root:
            return
        day = time.strftime('%Y%m%d', time.gmtime(record['time']))
        if day != self.day:
            try:
                self.machines, self.spent = self.usage(day)
            except OSError:
                return
            self.day = day
        if name not in self.machines and name != UNKNOWN:
            if len(self.machines) >= RECORD_MACHINES:
                record = dict(record, id=name)
                name = UNKNOWN
            else:
                self.machines.add(name)
        line = (json.dumps(record, separators=(',', ':')) + '\n').encode()
        path = os.path.join(self.root, name, day + '.jsonl')
        try:
            size = os.path.getsize(path)
        except OSError:
            size = 0
        if size + len(line) > RECORD_FILE_LIMIT or self.spent + len(line) > RECORD_DAY_LIMIT:
            return
        try:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, 'ab') as stream:
                stream.write(line)
        except OSError:
            return
        self.spent += len(line)

    def count(self, name, record, size):
        key = (name, tuple(sorted(record.items())))
        if key not in self.counters and len(self.counters) >= RECORD_COUNTERS:
            key = (UNKNOWN, (('event', 'uncounted'),))
        counter = self.counters.setdefault(key, [0, 0])
        counter[0] += 1
        counter[1] += size

    def flush(self, now, force=False):
        if not force and now - self.flushed < RECORD_PERIOD:
            return
        period, self.flushed = now - self.flushed, now
        counters, self.counters = self.counters, {}
        for (name, items), (packets, size) in counters.items():
            self.write(name, dict(items, time=round(time.time(), 3), packets=packets, bytes=size, period=round(period, 1)))

    def prune(self, now):
        if not self.root or (self.pruned is not None and now - self.pruned < RECORD_PRUNE):
            return
        self.pruned = now
        oldest = time.strftime('%Y%m%d', time.gmtime(time.time() - RECORD_DAYS * 86400))
        try:
            for name in os.listdir(self.root) if os.path.isdir(self.root) else []:
                folder = os.path.join(self.root, name)
                if not os.path.isdir(folder):
                    continue
                for entry in os.listdir(folder):
                    if entry.endswith('.jsonl') and entry[:-6] < oldest:
                        os.remove(os.path.join(folder, entry))
                if not os.listdir(folder):
                    os.rmdir(folder)
        except OSError:
            pass


def peer(address):
    return '%s:%d' % address


class Relay:
    def __init__(self, sock, status_path, record_path=None):
        self.sock = sock
        self.status_path = status_path
        self.recorder = Recorder(record_path)
        self.devices = {}
        self.clients = {}
        self.rates = {}
        self.window = time.monotonic()
        self.status_time = 0.0
        self.secret = os.urandom(32)

    def cookie(self, key, address, period):
        text = b'%s|%d|%d|' % (address[0].encode(), address[1], period)
        return hmac.new(self.secret, text + key, hashlib.sha256).digest()[:COOKIE_SIZE]

    def routable(self, data, key, address):
        period = int(time.time() / COOKIE_PERIOD)
        offered = data[HEADER_SIZE:HEADER_SIZE + COOKIE_SIZE]
        if any(hmac.compare_digest(offered, self.cookie(key, address, p)) for p in (period, period - 1)):
            return True
        self.sock.sendto(data[:HEADER_SIZE] + self.cookie(key, address, period), address)
        self.recorder.count(key.hex(), {'event': 'challenge', 'source': peer(address)}, len(data))
        return False

    def allowed(self, address, now):
        if now - self.window >= RATE_WINDOW:
            self.rates.clear()
            self.window = now
        count = self.rates.get(address[0], 0) + 1
        self.rates[address[0]] = count
        return count <= RATE_PACKETS

    def expire(self, now):
        for key in [k for k, d in self.devices.items() if now - d['seen'] > DEVICE_TIMEOUT]:
            self.recorder.write(key.hex(), {'time': round(time.time(), 3), 'event': 'forgotten', 'silent': round(now - self.devices[key]['seen'], 1)})
            del self.devices[key]
        for key in list(self.clients):
            live = {a: t for a, t in self.clients[key].items() if now - t <= CLIENT_TIMEOUT}
            if live:
                self.clients[key] = live
            else:
                del self.clients[key]

    def drop(self, name, reason, data, address):
        self.recorder.count(name, {'event': 'dropped', 'reason': reason, 'source': peer(address)}, len(data))

    def announced(self, key, device, address, now):
        state = (peer(address), device['mac'], device['flags'])
        last = device.get('recorded')
        if last and last[0] == state and device['uptime'] >= last[1] and now - last[2] < RECORD_PERIOD:
            return
        device['recorded'] = (state, device['uptime'], now)
        self.recorder.write(key.hex(), {'time': round(time.time(), 3), 'event': 'announce', 'source': state[0], 'mac': device['mac'],
                                        'uptime': device['uptime'], 'output': device['output'], 'flags': device['flags'],
                                        'attached': device['attached'], 'stopped': device['stopped'], 'crashed': device['crashed'],
                                        'restarted': bool(last and device['uptime'] < last[1])})

    def handle(self, data, address, now):
        if len(data) < HEADER_SIZE or len(data) > MAXIMUM_DATAGRAM or data[:4] != MAGIC or data[4] != VERSION:
            self.drop(UNKNOWN, 'not a debugger packet', data, address)
            return
        if not self.allowed(address, now):
            self.drop(UNKNOWN, 'rate limit', data, address)
            return
        kind = data[5]
        key = data[8:16]
        device = self.devices.get(key)
        clients = self.clients.get(key, {})

        if kind == TYPE_HELLO:
            if len(data) != HELLO_SIZE:
                self.drop(UNKNOWN, 'bad announcement size', data, address)
                return
            if device is None:
                if len(self.devices) >= MAXIMUM_DEVICES:
                    self.drop(UNKNOWN, 'too many machines', data, address)
                    return
                device = self.devices[key] = {'first': time.time()}
            flags, uptime, output = struct.unpack('<HIQ', data[42:56])
            device.update(address=address, seen=now, wall=time.time(), hello=data,
                          mac='-'.join('%02x' % b for b in data[36:42]), flags=flags,
                          attached=bool(flags & 1), stopped=bool(flags & 4), crashed=bool(flags & 8),
                          uptime=uptime, output=output)
            self.announced(key, device, address, now)
            for client in clients:
                self.sock.sendto(data, client)
        elif kind == TYPE_SUBSCRIBE:
            if len(data) != HELLO_SIZE:
                self.drop(UNKNOWN, 'bad subscription size', data, address)
                return
            if not self.routable(data, key, address):
                return
            if address not in clients and (len(clients) >= MAXIMUM_CLIENTS or
                                           (not clients and len(self.clients) >= MAXIMUM_DEVICES)):
                self.recorder.write(key.hex(), {'time': round(time.time(), 3), 'event': 'subscribe', 'source': peer(address), 'accepted': False})
                return
            if address not in clients:
                self.recorder.write(key.hex(), {'time': round(time.time(), 3), 'event': 'subscribe', 'source': peer(address), 'accepted': True,
                                                'machine': device is not None})
            self.clients.setdefault(key, {})[address] = now
            if device is not None:
                self.sock.sendto(device['hello'], address)
        elif kind in (TYPE_INIT, TYPE_RESPONSE, TYPE_DATA):
            if device is None:
                self.drop(UNKNOWN, 'no such machine', data, address)
                return
            event = 'data' if kind == TYPE_DATA else 'handshake'
            if address == device['address']:
                device['seen'] = now
                self.recorder.count(key.hex(), {'event': event, 'from': 'machine', 'clients': len(clients)}, len(data))
                for client in clients:
                    self.sock.sendto(data, client)
            elif address in clients:
                clients[address] = now
                self.recorder.count(key.hex(), {'event': event, 'from': 'debugger', 'source': peer(address)}, len(data))
                self.sock.sendto(data, device['address'])
            else:
                self.drop(key.hex(), 'sender not subscribed', data, address)

    def write_status(self, now):
        if not self.status_path or now - self.status_time < 2.0:
            return
        self.status_time = now
        devices = [{'id': key.hex(), 'address': d['address'][0], 'mac': d['mac'],
                    'first_seen': d['first'], 'last_seen': d['wall'], 'uptime': d['uptime'],
                    'attached': d['attached'], 'stopped': d['stopped'], 'crashed': d['crashed'], 'output': d['output'],
                    'clients': len(self.clients.get(key, {}))}
                   for key, d in self.devices.items()]
        temporary = self.status_path + '.tmp'
        with open(temporary, 'w') as stream:
            json.dump({'updated': time.time(), 'devices': devices}, stream)
        os.replace(temporary, self.status_path)

    def run(self):
        self.sock.settimeout(1.0)
        while True:
            try:
                data, address = self.sock.recvfrom(2048)
            except socket.timeout:
                data = None
            now = time.monotonic()
            if data is not None:
                self.handle(data, address, now)
            self.expire(now)
            self.write_status(now)
            self.recorder.flush(now)
            self.recorder.prune(now)


def main():
    parser = argparse.ArgumentParser(description='Relay between LiberNT debug targets and debugger clients.')
    parser.add_argument('--bind', default='0.0.0.0', help='address to listen on')
    parser.add_argument('--port', type=int, default=50000, help='UDP port to listen on')
    parser.add_argument('--status', help='file that receives the list of targets as JSON')
    parser.add_argument('--record', help='folder that keeps what each target sent, one file per target and day, for %d days' % RECORD_DAYS)
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((args.bind, args.port))
    signal.signal(signal.SIGTERM, lambda number, frame: sys.exit(0))
    relay = Relay(sock, args.status, args.record)
    try:
        relay.run()
    except KeyboardInterrupt:
        pass
    finally:
        relay.recorder.flush(time.monotonic(), force=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
"""Protocol edge cases against a scripted stub: NAK retransmit, bad
checksums, console output while running, monitor output, reply timeouts,
link loss and reconnect, and a gdb client on the seat."""

import os
import socket
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(__file__))
from harness import Checker, Hub, checksum, frame, free_port  # noqa: E402


class FakeStub:
    """Minimal all-stop stub with 4 KiB of memory at 0x1000."""

    def __init__(self, port):
        self.port = port
        self.mem = bytearray(range(256)) * 16
        self.running = False
        self.nak_once = {b"m1000,4"}  # answer '-' the first time we see these
        self.corrupt_once = {b"g"}    # reply with a bad checksum the first time
        self.silent = {b"m2000,4"}    # never answer (forces a hub timeout)
        self.seen = []
        self.last = b""
        self.naks_seen = 0
        self.lsock = socket.socket()
        self.lsock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.lsock.bind(("127.0.0.1", port))
        self.lsock.listen(1)
        self.conn = None
        self.stop_flag = False
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def send(self, payload: bytes, corrupt=False):
        f = frame(payload)
        self.last = f
        if corrupt:
            f = f[:-2] + (b"00" if f[-2:] != b"00" else b"11")
        self.conn.sendall(f)

    def reply(self, p: bytes):
        if p.startswith(b"qSupported"):
            self.send(b"PacketSize=400;QStartNoAckMode-")
        elif p == b"?":
            self.send(b"S05")
        elif p == b"g":
            self.send(b"11111111" + b"22222222")
        elif p.startswith(b"m"):
            addr, n = (int(x, 16) for x in p[1:].split(b","))
            if addr < 0x1000 or addr + n > 0x2000:
                self.send(b"E14")
            else:
                self.send(self.mem[addr - 0x1000: addr - 0x1000 + n].hex().encode())
        elif p.startswith(b"M"):
            head, data = p[1:].split(b":")
            addr, n = (int(x, 16) for x in head.split(b","))
            self.mem[addr - 0x1000: addr - 0x1000 + n] = bytes.fromhex(data.decode())
            self.send(b"OK")
        elif p.startswith(b"qRcmd,"):
            text = bytes.fromhex(p[6:].decode())
            self.send(b"O" + (b"echo: " + text + b"\n").hex().encode())
            self.send(b"OK")
        elif p in (b"c", b"s") or p.startswith(b"vCont;"):
            if p == b"s":
                self.send(b"T05thread:01;")
                return
            self.running = True
            self.send(b"O" + b"hello from target\n".hex().encode())
        elif p.startswith(b"Z0") or p.startswith(b"z0"):
            self.send(b"OK")
        else:
            self.send(b"")

    def serve(self):
        while not self.stop_flag:
            try:
                self.conn, _ = self.lsock.accept()
            except OSError:
                return
            buf = b""
            while True:
                try:
                    data = self.conn.recv(4096)
                except OSError:
                    break
                if not data:
                    break
                buf += data
                while buf:
                    if buf[:1] == b"-":
                        self.naks_seen += 1
                        self.conn.sendall(self.last)
                        buf = buf[1:]
                        continue
                    if buf[:1] == b"+":
                        buf = buf[1:]
                        continue
                    if buf[:1] == b"\x03":
                        buf = buf[1:]
                        if self.running:
                            self.running = False
                            self.send(b"T02thread:01;")
                        continue
                    if buf[:1] != b"$":
                        buf = buf[1:]
                        continue
                    end = buf.find(b"#")
                    if end < 0 or len(buf) < end + 3:
                        break
                    p, cs = buf[1:end], buf[end + 1:end + 3]
                    buf = buf[end + 3:]
                    if cs != checksum(p):
                        self.conn.sendall(b"-")
                        continue
                    if p in self.nak_once:
                        self.nak_once.discard(p)
                        self.conn.sendall(b"-")
                        continue
                    self.conn.sendall(b"+")
                    self.seen.append(p)
                    if p in self.silent:
                        continue
                    if p in self.corrupt_once:
                        self.corrupt_once.discard(p)
                        self.send(b"11111111" + b"22222222", corrupt=True)
                        continue
                    self.reply(p)
            self.conn.close()

    def drop(self):
        """Close the current connection; keep listening."""
        try:
            self.conn.shutdown(socket.SHUT_RDWR)
        except (OSError, AttributeError):
            pass

    def close(self):
        self.stop_flag = True
        self.drop()
        self.lsock.close()


class GdbClient:
    """Speaks just enough RSP to act as a human's gdb."""

    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.buf = b""

    def packet(self, payload: bytes, wait_reply=True):
        self.s.sendall(frame(payload))
        return self.read_packet() if wait_reply else None

    def read_packet(self):
        while True:
            while self.buf[:1] in (b"+", b"-"):
                self.buf = self.buf[1:]
            start = self.buf.find(b"$")
            end = self.buf.find(b"#", start + 1) if start >= 0 else -1
            if start >= 0 and end >= 0 and len(self.buf) >= end + 3:
                p = self.buf[start + 1:end]
                self.buf = self.buf[end + 3:]
                self.s.sendall(b"+")
                return p
            chunk = self.s.recv(4096)
            if not chunk:
                raise ConnectionError("seat closed")
            self.buf += chunk

    def close(self):
        self.s.close()


def main(cogdb):
    c = Checker()
    stub_port = free_port()
    stub = FakeStub(stub_port)
    hub = Hub(cogdb, f"tcp:127.0.0.1:{stub_port}", extra=("--timeout", "600"))
    try:
        hub.wait_status("state", "stopped")
        c.ok(True, "link up, initial '?' gives stopped")

        r = hub.ctl("MEM 0x1000 4")
        c.ok(r.startswith("OK") and "00010203" in r, "stub NAK is answered by retransmit", r)

        r = hub.ctl("REGS")
        c.ok(r.startswith("OK") and "g=1111111122222222" in r and stub.naks_seen == 1,
             "bad checksum from stub is NAKed and resent; no tdesc gives raw g", r)

        r = hub.ctl("MEM 0x1ffe 8")
        c.ok(r.startswith("ERR"), "out-of-range read reports the stub error", r)

        r = hub.ctl("MEM 0x2000 4")
        c.ok("did not answer" in r, "silent stub times out", r)
        r = hub.ctl("MEM 0x1004 2")
        c.ok(r.startswith("OK") and "0405" in r, "session usable after a timeout", r)

        r = hub.ctl("WMEM 0x1010 cafe")
        r2 = hub.ctl("MEM 0x1010 2")
        c.ok(r.startswith("OK") and "cafe" in r2, "memory write round-trips", r + r2)

        r = hub.ctl("MONITOR reset halt")
        c.ok(r.startswith("OK") and "echo: reset halt" in r, "monitor output collected", r)

        r = hub.ctl("RAW c")
        c.ok(r.startswith("ERR"), "RAW refuses resume packets", r)

        r = hub.ctl("CONT")
        c.ok(r.startswith("OK running"), "agent continue", r)
        hub.wait_status("state", "running")
        deadline = time.time() + 3
        while time.time() < deadline:
            ev = hub.ctl("EVENTS 20 console")
            if "hello from target" in ev:
                break
            time.sleep(0.05)
        c.ok("hello from target" in ev, "console output while running becomes an event", ev)
        r = hub.ctl("MEM 0x1000 4")
        c.ok("running" in r, "memory read refused while running", r)
        r = hub.ctl("HALT 2000")
        c.ok(r.startswith("OK stopped") and "SIGINT" in r, "agent halt", r)

        # A gdb client on the seat: ack, no-ack negotiation, relay, policy.
        g = GdbClient(hub.gdb_port)
        sup = g.packet(b"qSupported:multiprocess+")
        c.ok(b"QStartNoAckMode+" in sup, "seat advertises no-ack mode to gdb", repr(sup))
        c.ok(g.packet(b"QStartNoAckMode") == b"OK", "no-ack answered on the gdb hop")
        c.ok(g.packet(b"m1000,2") == b"0001", "gdb memory read relayed")
        c.ok(g.packet(b"QNonStop:1") == b"E01", "non-stop refused")
        hub.wait_status("gdb", hub.status()["gdb"])
        c.ok(hub.status()["agent_level"] == "halt", "agent level drops while gdb attached")
        r = hub.ctl("STEP")
        c.ok(r.startswith("ERR policy"), "agent step refused while gdb attached", r)

        g.packet(b"c", wait_reply=False)
        out = g.read_packet()
        c.ok(out.startswith(b"O"), "gdb receives console output while running", repr(out))
        hub.wait_status("run_owner", "gdb")
        r = hub.ctl("HALT 2000")
        stop = g.read_packet()
        c.ok(r.startswith("OK stopped") and stop.startswith(b"T02"),
             "agent halt delivers the stop to gdb", r + repr(stop))

        g2 = socket.create_connection(("127.0.0.1", hub.gdb_port), timeout=5)
        g2.settimeout(2)
        c.ok(g2.recv(16) == b"", "second gdb is refused")
        g2.close()

        c.ok(g.packet(b"D") == b"OK", "detach answered by the hub")
        g.close()
        hub.wait_status("gdb", "-")
        hub.wait_status("state", "running")
        c.ok(True, "detach resumes the target (default policy)")
        hub.ctl("HALT 2000")

        r = hub.ctl("BREAK 0x1000")
        c.ok(r.startswith("OK") and "kind=4" in r, "breakpoint with default kind", r)
        c.ok("count=1" in hub.ctl("BREAKS"), "breakpoint tracked")

        stub.drop()
        hub.wait_status("link", "down")
        c.ok(hub.status()["agent_breakpoints"] == "0", "breakpoints forgotten on link loss")
        hub.wait_status("link", "up", timeout=5)
        hub.wait_status("state", "stopped")
        c.ok(True, "hub reconnects after link loss")
        ev = hub.ctl("EVENTS 50 link")
        c.ok("down" in ev and ev.count("up") >= 2, "link events recorded", ev)
    finally:
        rc = hub.close()
        stub.close()
    c.ok(rc == 0, "hub exits cleanly on SIGTERM", str(rc))
    return c.done()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))

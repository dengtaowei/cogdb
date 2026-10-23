"""Shared helpers for the Python tests: run a hub in an isolated runtime
dir and talk to its control socket directly."""

import os
import socket
import subprocess
import sys
import tempfile
import time


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def checksum(payload: bytes) -> bytes:
    return b"%02x" % (sum(payload) & 0xFF)


def frame(payload: bytes) -> bytes:
    return b"$" + payload + b"#" + checksum(payload)


class Hub:
    def __init__(self, cogdb, stub, name="t", extra=()):
        self.cogdb = cogdb
        self.name = name
        self.tmp = tempfile.TemporaryDirectory(prefix="cogdb-test-")
        self.env = dict(os.environ, XDG_RUNTIME_DIR=self.tmp.name)
        self.gdb_port = free_port()
        self.log = open(os.path.join(self.tmp.name, "hub.log"), "w+")
        self.proc = subprocess.Popen(
            [cogdb, "serve", "--stub", stub, "-n", name, "-p", str(self.gdb_port), *extra],
            env=self.env, stdout=self.log, stderr=subprocess.STDOUT)
        self.sock = os.path.join(self.tmp.name, "cogdb", name, "control.sock")
        deadline = time.time() + 5
        while time.time() < deadline:
            if os.path.exists(self.sock):
                try:
                    self.ctl("PING")
                    return
                except OSError:
                    pass
            time.sleep(0.05)
        raise RuntimeError("hub did not come up:\n" + self.logs())

    def ctl(self, line, timeout=20.0):
        s = socket.socket(socket.AF_UNIX)
        s.settimeout(timeout)
        s.connect(self.sock)
        s.sendall(line.encode() + b"\n")
        out = b""
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            out += chunk
        s.close()
        return out.decode(errors="replace")

    def status(self):
        kv = {}
        for line in self.ctl("STATUS").splitlines()[1:]:
            k, _, v = line.partition("=")
            kv[k] = v
        return kv

    def wait_status(self, key, value, timeout=5.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.status().get(key) == value:
                return
            time.sleep(0.05)
        raise AssertionError(f"{key} never became {value}: {self.status()}")

    def cli(self, *args, timeout=30):
        return subprocess.run([self.cogdb, "-n", self.name, *args], env=self.env,
                              capture_output=True, text=True, timeout=timeout)

    def logs(self):
        self.log.flush()
        self.log.seek(0)
        return self.log.read()

    def close(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        rc = self.proc.returncode
        self.log.close()
        self.tmp.cleanup()
        return rc


class Checker:
    def __init__(self):
        self.failed = 0
        self.run = 0

    def ok(self, cond, what, detail=""):
        self.run += 1
        if not cond:
            self.failed += 1
            print(f"FAIL: {what}\n{detail}", file=sys.stderr)
        else:
            print(f"ok   {what}")

    def done(self):
        print(f"{self.run} checks, {self.failed} failed")
        return 1 if self.failed else 0

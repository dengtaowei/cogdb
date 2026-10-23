#!/usr/bin/env python3
"""End to end with real tools: gdbserver holds a process, cogdb holds
gdbserver, a real gdb and an agent share it."""

import json
import os
import re
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(__file__))
from harness import Checker, Hub, free_port  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))


def symbols(binary):
    out = subprocess.run(["nm", binary], capture_output=True, text=True, check=True).stdout
    syms = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            syms[parts[2]] = int(parts[0], 16)
    return syms


def main(cogdb):
    c = Checker()
    work = tempfile.TemporaryDirectory(prefix="cogdb-e2e-")
    exe = os.path.join(work.name, "spin")
    subprocess.run(["gcc", "-g", "-O0", "-no-pie", "-o", exe,
                    os.path.join(HERE, "fixtures", "spin.c")], check=True)
    syms = symbols(exe)
    stub_port = free_port()
    gs = subprocess.Popen(["gdbserver", f"127.0.0.1:{stub_port}", exe],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    record = os.path.join(work.name, "session.jsonl")
    hub = Hub(cogdb, f":{stub_port}", extra=("--record", record))
    try:
        hub.wait_status("state", "stopped")
        r = hub.ctl("REGS rip,rsp")
        c.ok(re.search(r"^rip=0x[0-9a-f]{16}$", r, re.M) is not None,
             "registers by name from gdbserver's target description", r)
        c.ok(hub.status()["arch"] == "i386:x86-64", "architecture learned")

        tick = syms["tick"]
        r = hub.ctl(f"BREAK {tick:#x}")
        c.ok(r.startswith("OK") and "kind=1" in r, "x86 breakpoint kind defaults to 1", r)
        r = hub.ctl("CONT 5000")
        c.ok("reason=swbreak" in r and f"pc=0x{tick:016x}" in r,
             "continue runs to the breakpoint and reports pc", r)
        r = hub.ctl(f"MEM {syms['banner']:#x} 13")
        c.ok("636f6764622d66697874757265" in r, "memory read returns the banner", r)
        hub.ctl(f"UNBREAK {tick:#x}")
        c.ok("count=0" in hub.ctl("BREAKS"), "breakpoint removed")

        cli = hub.cli("mem", hex(syms["banner"]), "16")
        c.ok(cli.returncode == 0 and "|cogdb-fixture" in cli.stdout, "CLI renders a hexdump",
             cli.stdout + cli.stderr)

        # Human gdb continues; the agent interrupts; gdb carries on as usual.
        gdb = subprocess.Popen(
            ["gdb", "-q", "-nx", "-batch",
             "-ex", "set sysroot /",
             "-ex", f"target remote 127.0.0.1:{hub.gdb_port}",
             "-ex", "continue",
             "-ex", "print counter > 0",
             "-ex", "detach", exe],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        hub.wait_status("run_owner", "gdb", timeout=20)
        c.ok(hub.status()["agent_level"] == "halt", "agent capped while gdb is attached")
        r = hub.ctl("CONT")
        c.ok(r.startswith("ERR policy"), "agent cannot resume behind gdb's back", r)
        r = hub.ctl("REGS")
        c.ok("running" in r, "agent cannot read while running", r)
        r = hub.ctl("HALT 3000")
        c.ok(r.startswith("OK stopped") and "SIGINT" in r, "agent halt", r)
        out, _ = gdb.communicate(timeout=30)
        c.ok("Program received signal SIGINT" in out and "$1 = 1" in out,
             "gdb sees the stop and keeps working", out)
        hub.wait_status("gdb", "-")
        hub.wait_status("state", "running")
        c.ok(True, "gdb detach leaves the target running")
        c.ok(hub.status()["agent_level"] == "write", "agent level restored after detach")

        gdb = subprocess.run(
            ["gdb", "-q", "-nx", "-batch",
             "-ex", "set sysroot /",
             "-ex", f"target remote 127.0.0.1:{hub.gdb_port}",
             "-ex", "monitor cogdb allow write 30",
             "-ex", "shell sleep 0.3",
             "-ex", "monitor cogdb status",
             "-ex", "detach", exe],
            capture_output=True, text=True, timeout=30)
        said = gdb.stdout + gdb.stderr
        c.ok("agents may write for 30s" in said and "agent level is now write" in said
             and re.search(r"for (29|30)s more", said) is not None,
             "real gdb shows `monitor cogdb` output", said)
        hub.wait_status("gdb", "-")
        c.ok(hub.status()["grant"] == "-", "grant gone after the real gdb detaches")

        r = hub.ctl("HALT 3000")
        r2 = hub.ctl(f"MEM {syms['counter']:#x} 8")
        c.ok(r.startswith("OK stopped") and r2.startswith("OK"), "agent alone again", r + r2)

        ev = hub.ctl("EVENTS 100")
        for needle in ("client gdb attached", "run gdb", "note agent interrupt",
                       "detach answered by hub", "client gdb detached"):
            c.ok(needle in ev, f"event log has '{needle}'", ev)

        with open(record) as fp:
            lines = [json.loads(line) for line in fp]
        pk = [e for e in lines if e["type"] == "packet"]
        c.ok(any(e["origin"] == "gdb" and e["dir"] == ">" for e in pk) and
             any(e["origin"] == "agent" for e in pk) and
             any(e["origin"] == "target" and e["dir"] == "<" for e in pk),
             "recording holds gdb, agent and target packets", str(len(pk)))

        gs.kill()
        gs.wait()
        hub.wait_status("link", "down")
        r = hub.ctl("REGS")
        c.ok("link is down" in r, "commands report a lost stub", r)
    finally:
        rc = hub.close()
        if gs.poll() is None:
            gs.kill()
        work.cleanup()
    c.ok(rc == 0, "hub exits cleanly", str(rc))
    return c.done()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))

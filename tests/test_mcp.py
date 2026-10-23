#!/usr/bin/env python3
"""MCP over stdio against a hub on the fake stub."""

import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(__file__))
from harness import Checker, Hub, free_port  # noqa: E402
from test_fake_stub import FakeStub  # noqa: E402


def main(cogdb):
    c = Checker()
    port = free_port()
    stub = FakeStub(port)
    hub = Hub(cogdb, f"127.0.0.1:{port}", name="mcp")
    mcp = subprocess.Popen([cogdb, "mcp", "-n", "mcp"], env=hub.env, stdin=subprocess.PIPE,
                           stdout=subprocess.PIPE, text=True)
    seq = [0]

    def rpc(method, params=None):
        seq[0] += 1
        msg = {"jsonrpc": "2.0", "id": seq[0], "method": method}
        if params is not None:
            msg["params"] = params
        mcp.stdin.write(json.dumps(msg) + "\n")
        mcp.stdin.flush()
        return json.loads(mcp.stdout.readline())

    def call(name, **args):
        res = rpc("tools/call", {"name": name, "arguments": args})["result"]
        return res["content"][0]["text"], res.get("isError", False)

    try:
        hub.wait_status("state", "stopped")
        init = rpc("initialize", {"protocolVersion": "2024-11-05"})
        c.ok(init["result"]["serverInfo"]["name"] == "cogdb", "initialize")
        names = [t["name"] for t in rpc("tools/list")["result"]["tools"]]
        c.ok("gdb_halt" in names and "gdb_read_memory" in names, "tools listed", str(names))

        text, err = call("gdb_status")
        c.ok(not err and "state=stopped" in text, "status tool", text)
        text, err = call("gdb_read_memory", address="0x1000", length=20)
        c.ok(not err and "0000000000001000: 00 01 02 03" in text, "memory as hexdump", text)
        text, err = call("gdb_read_memory", address="zz", length=4)
        c.ok(err, "bad address rejected before reaching the hub", text)
        text, err = call("gdb_continue")
        c.ok(not err and "running" in text, "continue", text)
        text, err = call("gdb_read_registers")
        c.ok(err and "running" in text, "errors surface as isError", text)
        text, err = call("gdb_halt")
        c.ok(not err and "SIGINT" in text, "halt", text)
        text, err = call("gdb_monitor", command="reg pc")
        c.ok(not err and "echo: reg pc" in text, "monitor", text)
        text, err = call("gdb_breakpoint", action="set", address="0x1000", kind=2)
        c.ok(not err and "kind=2" in text, "breakpoint set", text)
        text, err = call("gdb_events", types="stop,run")
        c.ok(not err and "SIGINT" in text, "events", text)
    finally:
        mcp.stdin.close()
        mcp.wait(5)
        hub.close()
        stub.close()
    return c.done()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))

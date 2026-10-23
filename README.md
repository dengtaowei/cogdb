# cogdb

**one debug stub, a human and an agent**

A GDB stub serves one session. OpenOCD, gdbserver, J-Link and a UART stub
all assume a single `target remote`. That breaks down on a desk where a
person is stepping through code in gdb and an AI agent wants to look at
the same core: read a register, halt a runaway loop, check a buffer.

cogdb holds the stub's one session and shares it at the protocol level:

```
  gdb (human)  ── target remote :4333 ──┐
  agent / MCP  ── control socket ───────┼─► cogdb ─► OpenOCD :3333 / gdbserver /
  logger       ── WATCH / --record ─────┘    (one RSP session)   serial stub
```

The human keeps an ordinary gdb with all of its features. The agent gets
small, structured commands (`HALT`, `REGS`, `MEM`, `BREAK`, `WAIT`), and
the hub schedules both onto the single RSP session.

It is the sibling of couart, which does the same for a UART console.

## What it does

- **Transparent seat for gdb.** Packets from gdb are relayed byte-exact,
  so `monitor`, `load`, `vFile`, thread and RTOS packets all work. The hub
  answers only what concerns the gdb↔hub hop: no-ack mode (offered even
  when a UART stub lacks it), non-stop (refused), and detach/kill.
- **Agent commands on the same session.** They are queued between gdb's
  packets. A command that needs a halted target says so instead of
  hanging.
- **Policy.** Agents get a level: `read < halt < run < write`. The level
  is capped while a gdb is attached (default: `halt`), so an agent can
  stop a runaway target but cannot resume it behind the human's back or
  scribble on memory gdb has cached.
- **Named registers.** The hub fetches `target.xml` over `qXfer`, so
  `REGS pc,sp,a0` works for RISC-V, ARM, x86 and so on.
- **Stop reports that agents can use.** `HALT`, `WAIT`, `STEP` and `CONT N`
  return `signal`, `reason` (swbreak/hwbreak/watch), the watch address,
  the thread and `pc`.
- **Events.** Link up/down, gdb attach/detach, run, stop, target console
  output (`O` packets) and timeouts all go to a ring (`EVENTS`), a live
  stream (`WATCH`), and optionally a JSON Lines recording of every packet
  (`--record`).
- **Robust link.** Acks, retransmit on `-`, NAK of bad checksums, reply
  timeouts, and reconnect with backoff when the stub restarts.
- **Transports.** `tcp:host:port`, `unix:/path`, `serial:/dev/ttyUSB0,115200`.
  A couart seat PTY works as a serial stub.
- **MCP server.** `cogdb mcp` exposes the commands as tools.

## Build

```bash
cmake -B build && cmake --build build
ctest --test-dir build --output-on-failure   # e2e needs gdb + gdbserver
```

C11, Linux, no dependencies beyond libc (cJSON is vendored).
`-DCOGDB_SANITIZE=ON` builds with ASan/UBSan.

## Use

```bash
# hold OpenOCD's session; gdb connects to 4333 instead of 3333
cogdb serve --stub :3333 --name myboard --record /tmp/myboard.jsonl

gdb-multiarch -ex 'target remote :4333' app.elf     # the human

cogdb -n myboard status
cogdb -n myboard halt
cogdb -n myboard regs pc,sp,ra
cogdb -n myboard mem 0x20000000 64
cogdb -n myboard break 0x80001234 hw
cogdb -n myboard cont 5000            # resume, wait up to 5 s for a stop
cogdb -n myboard monitor reset halt
cogdb -n myboard watch                # live event stream
```

`cogdb --help` lists every option.

### Policy

| Level   | Adds                                                   |
|---------|--------------------------------------------------------|
| `read`  | status, events, wait, registers, memory (when halted)  |
| `halt`  | interrupting a running target                          |
| `run`   | continue, step, breakpoints and watchpoints            |
| `write` | memory/register writes, monitor, raw packets, kick     |

`--agent LEVEL` applies with no gdb attached (default `write`), and
`--agent-with-gdb LEVEL` while one is (default `halt`).

### Detach

When gdb detaches or quits, the hub answers locally and the stub session
stays up for the agent. `--detach resume` (default) resumes the target,
`keep` leaves it halted, and `forward` passes `D`/`k` to the stub.

## MCP

```json
{
  "mcpServers": {
    "cogdb": { "command": "/path/to/cogdb", "args": ["mcp", "-n", "myboard"] }
  }
}
```

Tools: `gdb_status`, `gdb_halt`, `gdb_continue`, `gdb_step`,
`gdb_wait_stop`, `gdb_read_registers`, `gdb_read_memory`,
`gdb_write_memory`, `gdb_write_register`, `gdb_breakpoint`, `gdb_monitor`,
`gdb_events`, `gdb_raw_packet`.

## Compared with

- [smolmux](https://github.com/magnusmalm/smolmux) brokers a gdb *process*
  (GDB/MI). cogdb sits one layer down, so the human keeps their own gdb.
- [rsgdb](https://github.com/DynamicDevices/rsgdb) is an RSP proxy with
  recording, for one client. cogdb borrows the recording idea and adds
  concurrent agents and policy.
- [Sernic](https://github.com/Rybasum/Sernic) muxes a serial port with a
  raw GDB channel. cogdb parses RSP, so it can share the session safely.
- openocd-mcp and stm32-gdb-mcp give an agent its own debugger. cogdb lets
  an agent join the human's session instead.

Docs: [design](docs/DESIGN.md), [control protocol](docs/PROTOCOL.md).

## Status

v0.1: all-stop targets, one gdb client at a time, Linux.

## License

MIT. See [LICENSE](LICENSE).

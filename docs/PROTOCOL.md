# Control protocol

Each hub listens on a Unix stream socket (mode 0600):

```
$XDG_RUNTIME_DIR/cogdb/<name>/control.sock
```

The CLI, `cogdb mcp` and outside tools all use it. `HELLO` reports
`proto=1` for everything below.

## Framing

- Connect, send one line ending in `\n`, read until EOF. One command per
  connection. Later lines are ignored.
- A reply starts with `OK` or `ERR <reason>`, followed by command-specific
  lines.
- Command names are case-insensitive. Numbers are decimal or `0x` hex;
  addresses are hex with or without `0x`.
- Lines may be up to 256 KiB, and at most 32 connections can be open at
  once.
- A command may wait on the target (`HALT`, `WAIT`, `MEM`, …). Keep the
  connection open until EOF.

## Gating

Before a command runs, the hub checks:

1. **Policy level.** `ERR policy: CMD needs level L, agents have M [while
   gdb is attached]`.
2. **Target state.** `ERR stub link is down`, `ERR target running (HALT
   first)`, `ERR target is exited`.

| Command   | Level | Needs   | Reply                                          |
|-----------|-------|---------|------------------------------------------------|
| `HELLO`   | read  |         | `OK cogdb=V proto=N` + `cmds=...`              |
| `PING`    | read  |         | `OK pong`                                      |
| `STATUS`  | read  |         | `OK` + `key=value` lines                       |
| `EVENTS [N] [TYPES]` | read |  | `OK` + event lines                         |
| `WATCH [TYPES]` | read |      | `OK watching`, then event lines until close    |
| `BREAKS`  | read  |         | `OK count=N` + `TYPE 0xADDR kind=K` lines      |
| `WAIT [MS]` | read | link   | stop report, or `ERR timeout: ...`             |
| `REGS [all\|N,N]` | read | halted | `OK` + `name=value` lines              |
| `REG NAME` | read | halted  | `OK` + `name=value`                            |
| `MEM ADDR LEN` | read | halted | `OK addr=0x.. bytes=N [partial=1]` + hex    |
| `HALT [MS]` | halt | link   | stop report                                    |
| `CONT [MS]` | run | halted  | `OK running`, or with MS a stop report         |
| `STEP`    | run   | halted  | stop report                                    |
| `BREAK ADDR [TYPE] [KIND]` | run | halted | `OK addr=0x.. kind=K`           |
| `UNBREAK ADDR [TYPE] [KIND]` | run | halted | `OK addr=0x.. kind=K`         |
| `WMEM ADDR HEX` | write | halted | `OK wrote=N`                            |
| `WREG NAME VALUE` | write | halted | `OK`                                  |
| `MONITOR TEXT` | write | idle | `OK` + monitor output                        |
| `RAW PACKET` | write | idle  | `OK bytes=N` + reply (non-printables `\xNN`)  |
| `KICK`    | write |         | `OK kicked` (drops the gdb client)             |
| `RECONNECT` | write |       | `OK link=up\|down`                             |
| `SHUTDOWN` | write |        | `OK shutting down`                             |

"Halted" means stopped. "Idle" means linked and not running.

### Stop report

```
OK stopped | OK stepped | OK already <state> | OK <state>   (first line)
state=stopped
stop=signal=5 (SIGTRAP) reason=swbreak thread=p1.1 core=0
pc=0x0000000080001234
```

`stop=` is one of:

- `signal=N (NAME) [reason=R] [addr=0x..] [thread=T] [core=C]`
- `exited code=N`
- `terminated signal=N (NAME)`
- `-` (no stop seen yet)

`reason` is the stub's: `swbreak`, `hwbreak`, `watch`, `rwatch`,
`awatch`, … `pc=` is present when the hub knows which register is the
program counter.

### Breakpoint types

`TYPE` is `sw` (Z0, default), `hw` (Z1), `write` (Z2), `read` (Z3), or
`access` (Z4). `KIND` is the instruction length for sw/hw (default 1 on
x86, 4 elsewhere; use 2 for compressed RISC-V or Thumb) and the byte
length for watchpoints (default 4).

### Registers

Names come from the stub's target description. Values are shown as
numbers (`0x…`, using `--big-endian` if set) for registers up to 64 bits,
and as raw target-order hex for wider ones. `unavailable` means the stub
sent `xx`. `REGS` with no argument lists registers up to 64 bits; `all`
lists everything. With no target description, the reply is `g=<raw hex>`.

### STATUS keys

`name stub link state run_owner last_stop gdb gdb_port detach agent_level
policy queue arch registers packet_size agent_breakpoints events record`

### Events

```
<seq> <seconds.ms since hub start> <type> <origin> <text>
```

Types: `link client run stop console note packet`. `TYPES` is a comma
list of these, or `all`. The default is everything except `packet`.
Origins: `gdb`, `agent`, `hub`, `target`. Unprintable bytes are written as
`\xNN` and newlines as `\n`. Packet lines carry a direction, `>` (toward
the stub) or `<`.

## Versioning

`proto` goes up when a reply changes shape or a command changes meaning.
New commands do not bump it; check `cmds=` from `HELLO`.

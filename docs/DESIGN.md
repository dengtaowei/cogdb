# Design

## The problem in protocol terms

RSP is strictly request/response while the target is halted: one packet
in flight, one reply. Two exceptions shape everything else:

- A resume (`c`, `s`, `vCont;…`) has no reply until the target stops. The
  reply is a stop packet that can arrive seconds or hours later, with `O`
  console packets and `F` file-I/O requests in between.
- `0x03` (interrupt) is sent outside any packet while the target runs.

So sharing a session needs a scheduler, not a byte mux: send one request
at a time, route each reply back to whoever asked, and keep track of
whether the target runs and who resumed it. That scheduler is
`hub/target.c`. Everything else is input or output for it.

## Layers

```
 cli/        main.c (CLI)   mcp.c (MCP tools)   client.c (socket client)
              │ control socket, line protocol
 hub/        hub.c ── composition root
              ├── ctl.c       socket server, call lifetime, policy/state gate
              │   ctl_cmd.c   command table + handlers
              ├── gdbseat.c   gdb's TCP seat, local answers for this hop
              ├── regmap.c    target.xml fetch, register naming/formatting
              ├── record.c    JSON Lines recorder
              ├── policy.c    agent levels
              ├── grant.c     time-limited raises the gdb user hands out
              ├── target.c    THE session: queue, acks, run state, timeouts
              └── events.c    publish/subscribe bus + event ring
 rsp/        rsp.c (framing, RLE, escapes)  stop.c (stop replies)  tdesc.c (XML)
 transport/  transport.c (registry)  tcp.c  unix.c  serial.c
 core/       loop.c (poll reactor, timers)  conn.c (buffered non-blocking fd)
 util/       buf  hex  log  sys
```

Dependencies point downward only. CMake builds each layer as its own
static library and declares only the downward links. `rsp/` and `util/`
are pure functions with no I/O; `tests/test_unit.c` covers them directly.

## Responsibilities

| Module      | Owns                                         | Does not know about        |
|-------------|----------------------------------------------|----------------------------|
| `target`    | stub link, request queue, run state, stop    | gdb, agents, policy        |
| `gdbseat`   | gdb's socket, ack mode on that hop, detach   | agents, control commands   |
| `ctl`       | control connections, gating, call cleanup    | packet formats             |
| `ctl_cmd`   | what each command sends and how it replies   | sockets, policy checks     |
| `regmap`    | target description, byte order, naming       | who asks                   |
| `events`    | delivery and the history ring                | producers and consumers    |

Modules that react to something another module did subscribe to the bus
instead of being called. When the link drops, `target` publishes a `link`
event. The gdb seat kicks its client, `regmap` forgets the register map,
and the hub clears the agent breakpoint list. `target` knows none of them.

## Requests and replies

```c
cg_target_submit(t, "agent", "m20000000,40", n, &ops, ctx);
```

`ops.on_reply` runs exactly once: with the reply, a timeout, or link
loss. `ops.on_output` receives the `O`/`F` packets that arrive while a
resume or `qRcmd` is open. Owners that go away call
`cg_target_forget(t, ctx)`: their queued requests are dropped, and the one
already on the wire completes silently so the session stays in sync.

Every control command is a `cg_call_t`. A handler returns `DONE`,
`PENDING` (and finishes later from a callback) or `STREAM`. Bus
subscriptions, timers and per-call state registered on the call are
released when the call ends, whether it finished or the client hung up.
No handler writes its own cleanup path.

## Policy

Agents' rights are a single level, checked in `ctl.c` against the
`level` column of the command table before the handler runs. The level is
lowered while a gdb is attached. gdb keeps private beliefs (cached
registers, "the target is stopped", its own breakpoint list), and an agent
must not invalidate them without the human seeing it:

- `halt` is safe: gdb's pending `continue` simply gets its stop reply.
- `run` is not: gdb would think the target is still where it left it.
- `write` is not: gdb's register and memory caches go stale.

## Extension points

| To add…            | Do this                                                         |
|--------------------|-----------------------------------------------------------------|
| a stub transport   | a file exporting a `cg_transport_t`, one line in `transport.c`  |
| a control command  | a handler and a row in `cg_commands[]` (level, state, usage)    |
| an MCP tool        | a `plan_*` function and a row in `k_tools[]`                    |
| a packet answered on the gdb hop | a row in `k_local[]` in `gdbseat.c`               |
| an event consumer  | `cg_bus_subscribe` with a type mask                             |

## Non-goals (v0.1)

- Non-stop mode, and several gdb clients at once. The second would need
  per-client thread/stop bookkeeping, and the protocol gives no way to
  tell gdb about stops it did not cause.
- Rewriting gdb's packets. Breakpoints gdb inserts are its own business.
  The hub only tracks the ones agents insert (`BREAKS`).

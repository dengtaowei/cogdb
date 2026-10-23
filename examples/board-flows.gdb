# Example flows for a RISC-V board behind XuanTie DebugServer (:1025).
# Run by the hub as: gdb -batch -nx -x THIS -ex "target remote :PORT" -ex NAME
#   cogdb serve --stub 127.0.0.1:1025 --name myboard \
#     --flow-gdb riscv64-unknown-elf-gdb \
#     --flow-script examples/board-flows.gdb --flows load_fw,where_fw
# gdb runs from this script's directory. Any error inside a define stops it
# and makes gdb exit non-zero.

set confirm off
set pagination off

define load_fw
  # A core that ran firmware holds dirty D-cache lines (its stack); writes
  # that bypass the cache would be read back stale. Reset first.
  monitor reset halt
  flushregs
  file /path/to/firmware.elf
  load
  compare-sections
  info registers pc
end

define where_fw
  file /path/to/firmware.elf
  info registers pc
  bt
end

# Integrated wide forwarding and numeric byte ends

Source `481959ce6` combines the `4b8c2f880` ordinary checkpoint with the
release-only unused assertion variable annotation, numeric dynamic byte-range
ends, and wider fixed argument vectors forwarded through VARG/CALLM. The
forwarded result count remains bounded at three; argument count follows the
existing JIT slot limit and the actual source frame window.

The complete Lua-leaf contract passed on ARM64 and ARM64e in assertion and
release profiles. This includes nested calls, VARG/CALLM, child exits,
compound and sparse exits, normalized arguments, and return exits. Each
profile includes the restored VARG destination mutation: 115 semantic and
115 post-allocation rejections. Wider native witnesses cover 4, 8, 16 and 32
arguments; the near-limit frame follows the interpreted fallback oracle.

The wrapper restored ordinary assertion ARM64 without test helpers, profile
`8d3115f36821c5fba56c2f1c9efeea6dd8fcc765cb527673a07518e7f301002e`.
That build passed all 509 stock source tests and all 509 original v2 bytecode
tests. Evidence is in
`/Users/frityet/Scratch/ljmt-wide-numeric-byte-integration-20261030/`:
`source.txt`, `lua-leaf-four-profiles.log`, `stock-source.log` and
`stock-bytecode.log`.

This is an integrated focused checkpoint. The separately running full gate
at `9581ed718` has production, tests and CI identical to `4b8c2f880`, so it
does not validate these later additions. Main parity still carries the
completed `89226814d` production checkpoint. Numeric FORL children, captured
FFI prefix upvalues, integer byte indices and negative dynamic byte ends are
being developed or integrated separately; full x86_64 parity is not claimed.

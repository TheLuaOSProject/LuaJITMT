# x64 TSET Non-GC Barrier Skip

The x64 interpreter direct `TSETV`, `TSETB`, `TSETR`, and existing-key
`TSETS` paths now skip `lj_gc_pubtabtv_vm()` when the stored `TValue` is not a
GC object.

The direct-store predicate still rejects active marking, weak tables,
metatables, active MT paths, retiring arrays/nodes, forwarded slots, nil
previous values that need `__newindex` checks, and other helper-only cases.
Only the post-store black-parent publication call is skipped, and only after a
DynASM tag check equivalent to `tvisgcv()`.

GC-valued stores still call `lj_gc_pubtabtv_vm()` so GC2 publication and the
legacy black-to-gray repair remain intact. The helper-return paths are left
unchanged because they cover weak/MT/retiring/forwarded cases rather than the
hot direct store path.

Coverage:

- `tests/t-x64-tset-nongc-barrier.c` links the VM with
  `--wrap=lj_gc_pubtabtv_vm` and verifies that numeric direct interpreter
  `TSETB`, `TSETV`, `TSETR`, and `TSETS` stores to a black table do not call
  the helper, while equivalent table-valued stores still do.
- `tools/ci/lua_test.sh m5_x64_tset_nil_snapshot`

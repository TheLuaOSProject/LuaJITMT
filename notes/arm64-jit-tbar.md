# ARM64 JIT table barrier helper

The ARM64 `IR_TBAR` lowering now calls the existing
`lj_gc_tbar_trace_g(g, table, key)` helper, matching x64's GC2 protocol. It no
longer edits `GCtab.marked`, `GCtab.gclist`, or `g.gc.grayagain` directly. A
null key requests the table barrier; a key is materialized in the executing
TG's `tmptv2` root before the helper applies strong/weak-key policy.

The target-local lowering retains the stock call allocator and fixed `RID_GL`
carrier. Runtime key materialization precedes the move of GL into the first
argument register: the TValue builder may reuse that register. This avoids a
new protected-temporary abstraction or changing shared assembler behavior.

Trace admission is unchanged. This removes a missing runtime mechanism before
opening table IR; it does not claim native general table access, arbitrary
helper-call layouts, upvalue barriers, or complete JIT support.

## Evidence

`tools/ci/arm64_jit_tbar_contract.sh` executes the production lowering through
a test-only assembler wrapper. The wrapper uses a private ABI envelope and
does not bypass trace admission for any actual Lua program. Eight cases run
on both native arm64 and arm64e with a BTI-enabled library:

- A null-key barrier marks its table in active GC2 MARK.
- Constant and dynamic GC keys are marked for a strong table, and are not
  marked by the barrier for a weak-key table.
- Integer keys and one dynamic numeric key exercise the helper's early-out.
- `tmptv` remains intact, `tmptv2` holds the complete key, and the legacy
  grayagain pointer and black bit remain unchanged.
- Dynamic keys deliberately occupy x0, exposing argument-register reuse.

The companion contract also checks the actual emitted instructions. Restoring
the old ARM64 barrier in a disposable build makes the first live GC2-mark
assertion fail. The existing `arm64_jit_emitter_contract.sh` passes with its
global-field inventory reduced from seven to five accesses.

## Next boundary

The common recorder already routes active-MT indexed reads through rooted
`rec_idx_mt_shared_sample()` / `rec_idx_mt_shared_load()` and
`lj_tab_gettv_rooted()`. A useful next increment is a capability certificate
for that helper family: validate TMPREF input/output provenance, the helper
signature, post-call VLOAD/snapshot ordering, and legal live register/spill
placements independently. Keep raw table slot loads and stores closed until
their forwarding/publication mechanisms are ported. Avoid another literal
bytecode/IR/register array for each table-loop spelling.

The newer upstream `origin/v2.1` at `489bb089` leaves the x64 TBAR lowering and
helper ABI unchanged. No upstream merge was performed for this slice.

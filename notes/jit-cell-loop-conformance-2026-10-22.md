# Loop capture and original-bytecode JIT conformance

The recovered x64 runner exposed a real closure-conformance failure after the
initial original-bytecode import work: an `ipairs` loop creating closures lost
entries once its loop trace executed. Source code also failed, so passing the
509 stock cases alone did not establish this behavior. Further reductions
exposed independent imported repeat-loop and conditional-return failures.

Four narrow corrections are required together:

1. Pending raw-cell promotion now consumes the current SSA value through
   TMPREF. Its old null argument read the physical Lua stack. Loop unrolling
   copied that hidden read unchanged while iterator index/value SSA references
   advanced through PHIs, reusing an earlier iteration's cell and key.
   An already-cell incoming reference retains its existing identity.
2. FNEW now derives a promoted cell from the freshly returned closure's UREFC,
   subtracting the existing `GCupval.tv` offset. The previous mid-trace SLOAD
   was treated as a trace-entry value by loop optimization, even after that
   local's scope ended. The new reference depends on the actual allocation;
   the new closure is still private and its captures are known closed here.
3. Both snapshot cell classification and source-liveness analysis now include
   read-only local captures. Their remaining immutable-descriptor exclusions
   contradicted the existing shared-identity policy. Missing NORESTORE could
   restore a cell pointer using the generic pointer's trace tag; the existing
   immutable FNEW regression caught this as a TValue/GC type mismatch.
4. Synthetic jumps inserted by the original-bytecode normalizer retain the
   complete normalized frame bound. Conditional routing jumps do not end a
   local lifetime, and UCLO's A operand is a capture-closing floor rather than
   a live-register cutoff. Reusing zero or the UCLO floor as JMP A discarded
   current SSA values. One visible failure was the exact sequence
   `assert(branch(i, true) == i+1); assert(branch(i, false) == i)`.

`tests/t-jit-cell-loops.lua` covers captured iterator values, distinct cells
per iteration, mutable repeat-scope captures and the exact conditional-call
pattern. It runs the same source and genuine original-LuaJIT v2 little/big
endian fixtures. On an enabled x64 JIT it additionally requires trace-exit
events from the capture-loop roots, proving execution beyond recording.
The fixture generator reproduces those bytecode files with stock LuaJIT.

## Evidence

Using the x64 assertion build under Rosetta, all of the following passed:

- The focused source/v2 LE/v2 BE loop fixture, standalone imported repeat and
  branch reductions, and 40 executions of the original comprehensive imported
  cells fixture, each with default optimization, `-O0` and `-O-cse`.
- The complete original-bytecode C fixture: debug/stripped LE/BE, v4 dump and
  reload, forced collection while loading, malformed/error paths, sharing and
  execution before/after the multithreading activation boundary.
- All 509 source stock cases and all 509 cases with generated source compiled
  by original LuaJIT and imported as v2 bytecode.
- The full FNEW C fixture in default and `LJ_TEST_TRACED_FNEW=1` modes; source
  identity, recursive-return capture and threaded upvalue regressions in all
  three optimization modes above.

The isolated upstream candidate's earlier safe ARM64 and arm64e GC/entry
checks do not validate this later change. ARM64 revalidation belongs to the
new primary checkpoint. General ARM64 heap-effect recording remains closed;
these generic fixes do not open additional target admission. The importer
still has the explicit frame-size/expanded-jump/uncommon captured-window
limitations documented in `legacy-bytecode-cells.md`.

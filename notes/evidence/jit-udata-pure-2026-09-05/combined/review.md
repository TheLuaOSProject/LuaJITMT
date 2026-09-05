# Combined namespace receiver guard and pure userdata reuse

No interaction blocker was found for this exact combination. All 261 requested
runtime processes pass across normal, assertion and Clang-ASan builds, including
stock 387 JIT-off / 509 JIT-on in each. There were no build, test, timeout or
instrumentation failures in this combined run. Shared files/builds were untouched
and no commits were made. The earlier isolated handoff remains immutable: its
manifest hash and all 110 artifact identities were rechecked successfully.

## Exact source and validation boundary

The base is 9f68fa8d plus ROOT's five-line `recff_clib_index` receiver guard,
`lj_crecord.c` SHA-256
`d9117ff3214f258cc84648725effcd498f3d1e93a774a94efd65e81ed11f2bff`,
and the unchanged `candidate-v2.patch`, SHA-256
`c53b908a2f6bd57294452ad9de58db34e25db1ac4414a90c1d78bda2d0df614c`.
The combined `lj_opt_mem.c` remains
`00787841d7305b758edacb399e16fe5494b9d923525baa5f6b9523af7acbfc2c`.
All 224 runtime/generator inputs match across the three builds. Relative to the
shared source frozen at setup only `lj_opt_mem.c` differs; relative to the prior
isolated candidate only `lj_crecord.c` differs. Byte identities were checked
again after validation.

There are three successful build commands and 278 validation commands (92
normal, 92 assertion, 94 ASan, including two ASan nm checks). Each variant runs
87 runtime processes:

- Stock JIT-off/on (2).
- The exact permanent captured-receiver fixture's ten modes off/on with separate
  libraries exporting values 11 and 29 (20), including old root/installed side
  exits, exact preceding-store counts, wrong-subtype errors and namespace lifetime.
- Direct userdata's 36 mutation combinations with actual hoist and native-exit
  witnesses (36).
- Seven excluded userdata-effect bodies plus the approved direct scalar cdata
  payload store (8).
- Existing pure-cdata mutations, seven exclusions, installed side reentry and
  profiler callback (16).
- Existing phase gate, global GC worker, protected OOM, first attachment with
  LOOP and without LOOP (5).

The unchanged broad 88-process special-userdata matrix was not repeated. All
C lifecycle fixtures use exactly the runtime's helper flags. Normal uses the
default mixed build. Assertion and ASan use static unstripped builds with all
GC2/FUNC/TAB/ARENA/TRACE/XSAVE helpers; ASan is Clang -O1 runtime instrumentation
with `detect_leaks=1:abort_on_error=1`, without suppressions. Both changed runtime
objects contain ASan references and both host generators do not. Final command,
fixture/library/executable hashes, stock tree hashes and compiler flags are in
`*-results.json`, `*-build.json`, `test-input-identity.json`,
`runtime-input-identity.json` and `final-validation.json`.

## Interaction review

The receiver repair adds an `IR_EQ` guard against a typed userdata KGC before
exporting cached constants or extern addresses. `lj_ir_kgc` publishes the
namespace as a current-trace GC constant; normal trace KGC preservation retains
that object. The guard remains executable at root entry. Its strong namespace
edge also remains in the constants when the copied equality is eliminated.
The existing pointer guard from metamethod dispatch and the new typed KGC guard
are distinct; the direct CLibrary IR retains both, together with the complete
metatable/node/method chain at entry.

The reuse candidate neither forwards an equality as a new field authority nor
removes those entry guards. It only relaxes the XPOLL alias bound for the two
already qualified direct-userdata field shapes. Equal receiver/field operands
are still required for CSE. The new equality is already in the unchanged
whole-body allowlist and introduces no runtime allocation, call, store, callback
or poll. The additional KGC allocation occurs during recording; it does not
change the protected unroller's flag scope or its error cleanup.

Captured methods can bypass ordinary metamethod lookup, which is why ROOT's
receiver guard is necessary independently of this optimization. A changed
namespace or wrong userdata subtype must fail the retained identity guard
before the specialized cached operation. Root/side reentry and exact error/store
behavior pass under the combined source. Lifetime modes retain the specialized
namespace after user references disappear. The receiver repair does not widen
any accepted userdata/node provenance or permit the excluded effectful bodies.
Their actual native IR still retains the repeated metatable proof.

This review covers the interaction with the unchanged pure-loop scope. It does
not extend the prior proof to active-MT metamethod recording, arbitrary table
node chains, cache override semantics, full lockless progress or platform release
qualification. Canonical registration and integration remain with ROOT.

## Direct loop geometry; no repeated timing

Four additional native IR/mcode probes use the exact earlier direct-cost fixture
with count 80. These are shape probes, not performance measurements. The new
receiver guard increases direct CLibrary from 23 to 24 IR instructions and
278 to 298 mcode bytes. It adds only the typed namespace identity guard at entry.
The hot body still has no copied userdata metatable or immediate node load.
File, buffer and plain userdata retain their earlier IR and mcode sizes.

For all four probes, hot-loop assembly, registers, relative instruction byte
offsets and loop start page offsets match the earlier candidate. Only absolute
addresses are normalized in that comparison; short field offsets and opcodes
are unchanged. Loop page offsets are CLibrary 4032, file 3984, buffer 3968 and
plain 3984. Raw old-input hashes, current dumps, comparison rules and exact
normalized loops are retained in `ir-check-results.json`.

No timing was repeated because there was no hot-loop geometry change supporting
it. The earlier cost figures remain measurements of their original source, not
new measurements of this combination. The captured-file bimodality and rejected
collision-chain hypothesis from the prior study remain unresolved and unchanged.

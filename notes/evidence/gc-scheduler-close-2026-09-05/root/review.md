# Root review: restore synthetic shutdown before real scheduler cleanup

The exact three-line fixture patch 67b54d179d8a6fb4e95920662344c7737828bf1f86d47977c350367eb2b55f23
is accepted on runtime eb8a5b2f. The input is the previously corrected
publication fixture f476733106d7e2552c9e47757b35f1c1dc78cf8b3cb0d47c17f17ed58fe70dca;
the output is e9d2173e1279088d500b7bff816dae54e260b039ad5c5d6034f738d5ad2a1a27.
Runtime source and close-admission rules are unchanged by this integration.

Root read the terminal subtest, its synthetic shutdown setup, the actual close
admission code, the frozen patch and both debugger witness explanations.
The subtest sets mt_shutdown=1 to exercise terminal TG unlink from a different
universe's raw TLS. It successfully checks worker/deferred TG unlink and empty
retired-worker storage, then leaves that synthetic value set when calling
lua_close. The original claim witness observes that exact call returning zero,
which makes lua_close return without teardown. Uninterrupted ASan controls
report 131,280 leaked bytes in six allocations in that second universe.

The fix restores only the test's synthetic flag after all terminal assertions
and before the real close. Real admission must still claim the exact state and
establish its own shutdown protocol. A separate positive read-only witness
observes shutdown zero at entry, actual claim one, close_state with real
shutdown one, GC2 fini, and return to the existing post-close TLS/root checks.
It does not dereference the freed state afterward. Neither debugger result is
counted as a full test pass.

The owner's eight uninterrupted passes cover pristine597b, isolated fair597b,
exact793 and 793+fair, each in assertion/helper and target-only ASan settings.
All four ASan runs retain detect_leaks=1:abort_on_error=1 and have empty stderr.
These runtime/header/helper configurations remain separately identified. The
793 missing fixture-include compilation attempts are preserved separately;
their successful successors add only exact frozen fixture headers.

Root verifies all 103 final owner artifacts, applies the exact patch, and runs
the registered m3_gc2_worker_scheduler suite against freshly built default
eb8a5b2f source. All three components pass: the complete C scheduler fixture
and worker Lua modes with JIT off/on. All 225 runtime/generator/root-Makefile
inputs still equal the accepted fair combination before and after the build.
The canonical argv, environment, fixture/runner/archive/ELF hashes, stdout,
stderr and elapsed time are preserved. Command logging on canonical stderr
does not represent a sanitizer report.

The combined evidence is eight owner plus three canonical full-test passes.
This closes the demonstrated fixture cleanup leak without weakening shutdown,
GC, worker, lifetime or publication checks. It does not establish general
runtime leak freedom, the separate worker-bridge change, iterator progress,
cross-platform validation or release readiness. Original leaks and failed
admission witnesses remain retained in the owner evidence.

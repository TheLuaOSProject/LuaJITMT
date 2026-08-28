# ARM64 GDBJIT preparation checkpoint (2026-08-27)

## Scope

This checkpoint isolates optional GDB JIT metadata from the future first-side
publication suffix. It does not call the new split from `lj_trace.c`, change an
ARM64 recorder/native-entry gate, or admit production side recording. The
existing `lj_gdbjit_addtrace()` root/side call site remains in place and now
delegates to the split API.

## Ownership and ordering

`lj_gdbjit_preparetrace()` owns every operation that may fail before semantic
publication:

- it chooses the complete private `J->cur` image when the destination is the
  still-uninitialized `J->curfinal`, and otherwise reads the published trace;
- a side trace uses one nonwaiting SMR-reader attempt, resolves the current
  parent slot and, when supplied, requires pointer equality with the exact
  parent generation before copying its stack adjustment;
- it builds the complete ELF payload in private storage and allocates the
  descriptor entry with `lj_mem_new_nothrow()`; and
- parent contention/staleness, an overlong filename and allocation failure all
  return `NULL`, meaning optional metadata omission rather than a Lua error.

The caller owns a successful preparation until
`lj_gdbjit_committrace()` succeeds. Commit validates the destination and
prepared state, consumes the preparation's only commit attempt, performs one
descriptor try-lock, links/registers the already initialized entry and returns.
It does not allocate, free, enter SMR, wait, yield or raise an error. A failed
commit remains caller-owned and must be passed to `lj_gdbjit_aborttrace()`
after any non-abortable publication suffix. A successful commit transfers the
allocation to the trace; ordinary GDBJIT trace deletion releases it.

The future first-side caller must make that one commit attempt after the final
semantic edge CAS but before releasing its recorder token and retained body
authority. Only failed-preparation cleanup is deferred until after token
handoff, because abort no longer dereferences the trace destination.

The compatibility `lj_gdbjit_addtrace()` wrapper prepares, commits once and
aborts a failed commit. This preserves successful root registration while
making optional allocation failure nonthrowing.

## Filename bound

The source filename is the only unbounded ELF-object input and appears exactly
twice. Object construction first emits the fixed empty-filename form. Before
emitting any source-name byte, it proves room for both full copies plus the
worst-case padding at the one final pointer-alignment boundary. A name that
cannot satisfy the strict 4096-byte object-space bound omits GDB metadata; it
cannot reach either unchecked string copy or the old post-write overflow
assertion.

## Deterministic proof

`tests/t-arm64-jit-gdbjit-prepare.c` runs five fresh Lua universes through the
ordinary admitted ARM64 integer-loop root path:

1. normal preparation and descriptor registration;
2. a 1200-byte source-name payload that exercises both bounded copies and
   registers successfully;
3. a test-only one-shot nonthrowing-allocation omission;
4. a real descriptor try-lock miss while the test holds the process-global
   descriptor lock, followed by abort; and
5. an `@` chunk name containing 6000 payload bytes, rejected by the preflight
   bound.

Every case proves the root remains runnable and returns the expected result.
The normal and 1200-byte cases alone attach a GDB entry, call the registration
breakpoint exactly once, and observe the first/relevant/action descriptor state
fully published at that callback. Counters distinguish bound and allocation
omissions and prove that real descriptor contention performs one commit
attempt, no registration and exactly one abort.

An additional abort-only synthetic probe presents an unpublished destination
through `J->curfinal`, proves that preparation reads the private `J->cur` image,
accepts the exact current parent slot generation and rejects a distinct
impostor body carrying the same trace number. It also proves that a wrong
destination does not consume a preparation, the first exact-destination
descriptor attempt does, and replay after a real held-lock miss performs no
second descriptor operation.

A second private probe discovers the actual conservative filename boundary,
then repeats the adjacent accepted and rejected payload sizes under fresh
counters. The accepted preparation's debugger-visible `symfile_size` must equal
the exact copied object extent and end within a small alignment bound of the
fixed object capacity; the immediately next payload must be rejected before
allocation.

`tools/ci/arm64_jit_gdbjit_prepare_contract.sh` additionally checks that the
commit function contains exactly one descriptor try-lock call, no source loop,
and no allocator, free, SMR, blocking-lock, retry-yield, GC or error helper. It
pins the one-shot state change and trace/descriptor/action/callback/unlock order.
It verifies the side-recorder gate remains closed, compiles the GDBJIT
translation unit both with and without test instrumentation plus the fixture
with warnings as errors, runs the fixture twice on arm64 and twice on
arm64e+BTI, and restores the ordinary thin arm64 build with all GDBJIT-only
helpers absent.

Validated locally on native Apple Silicon macOS:

```text
t-arm64-jit-gdbjit-prepare OK
arm64_jit_gdbjit_prepare_contract OK: bounded registration and one-shot omissions ran on ARM64/ARM64e; ordinary ARM64 was restored
```

The existing unrelated `lj_ccall.c` unused-function warning remains present in
the archive build; both focused translation units are warning-clean under
`-Wall -Wextra -Werror`.

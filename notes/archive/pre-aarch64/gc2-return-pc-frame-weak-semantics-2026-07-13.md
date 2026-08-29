# GC2 return-PC frame validation and one-cycle weak semantics (2026-07-13)

## Release blocker and exact regression

`m2_arena_gcclose` deterministically failed its final `__mode="kv"` assertion
on `57dc92bf`.  The same failure is independently covered before `lua_close()`
by `tests/t-weak-modes.lua:120`: a key/value pair whose lexical scope ended was
still present after one explicit full collection.  System LuaJIT clears the
entry in that collection.  This was therefore a GC2 weak-liveness regression,
not close ordering or an invalid fixture assumption.

`git bisect run` over a clean build and `tests/t-weak-modes.lua` identified:

- good: `a77da662` (`Reclaim strings at explicit GC2 boundaries`);
- first bad: `147c85c3` (`Fix table successor read readiness`).

The first-bad stack hardening changed same-thread frame walks to use
`gc2_frame_prev_safe()`.  That validator required a Lua-style frame's callee
function to own the frame PC.  LuaJIT frame PCs are return PCs owned by the
caller, and a normal Lua-to-C/fast call has a Lua-style return-PC frame whose
callee has no Lua prototype.  Consequently every `collectgarbage("collect")`
root scan rejected its valid top C frame, selected the conservative `maxstack`
fallback, and marked dead lexical slots.  P_WEAK correctly observed those
spurious marks and retained the weak entry.

## Fix and lifetime proof

`GC2FrameScope` now has a distinct return-PC observation scope.  Frame
validation:

1. admits and validates the callee function as before;
2. resolves the return PC to its containing caller prototype;
3. holds a non-marking exact small-arena admission or HugeTab range-reader
   token while reading `pc[-1]` and deriving the previous frame;
4. releases the PC, prototype, and function scopes together after the caller
   has consumed the frame result.

The PC scope deliberately does not publish a semantic mark.  The caller
function encountered in the next stable frame owns the caller-prototype graph
edge.  This separation avoids turning a validation read into repeated SWEEP
root/recovery publication.  Failed admission, malformed geometry, HugeReader
overflow, or the existing bounded frame-walk limit still selects the
conservative `maxstack` safety fallback.  Remote/native/JIT-owned stacks retain
their pre-existing conservative path before frame decoding.

The temporary custom `lua_Alloc` boundary is unchanged: custom-allocator bodies
remain outside GC2 reclamation for b1.2, so an owner-stable return PC needs no
arena token there.

## Regression visibility

`threading.gcstats().thread_scan_frame_fallbacks` counts invalid/over-limit
frame walks that select `maxstack`.  The one-cycle weak test now performs a
nested Lua-to-Lua-to-C collection and requires this counter to remain exactly
unchanged.  This covers both the Lua-to-C callee/caller distinction and a return
PC owned by a different Lua prototype without weakening weak-table assertions.

## Validation

- clean default warning build: passed;
- final nested/counter `tests/t-weak-modes.lua`, JIT and `-joff`: 50
  repetitions per mode passed (earlier candidate forms also passed longer
  focused runs);
- `m2_arena_gcclose`, assertion build: passed;
- `m9_gc_stats`, JIT and `-joff`: passed;
- `m3_gc_active_thread_roots`, JIT and `-joff`, plus its C active-collect
  fixture: passed;
- `m6_jit_flush_gc_current_stack`: passed;
- full remote/JIT stack-resize fixture was also sampled.  Its 10-second worker
  ready timeout is presently nondeterministic in both untouched `57dc92bf` and
  this candidate under concurrent repository builds; successful candidate and
  baseline runs were observed.  No remote/native/JIT stack path was changed.

The unrelated current `m8_weak` aggregate stops earlier in the default-finalizer
stderr-output assertion; direct weak semantic coverage above reaches and passes
the release-blocking one-cycle case.

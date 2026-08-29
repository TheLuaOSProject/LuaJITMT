# Generation-aware TG-local JIT hotcount reset

Baseline: `61d2a639` (`jit: stage strict ARM64 root entry admission`).

This checkpoint prepares the recorder/hotloop substrate without enabling ARM64
recording or native entry. `LJ_ARM64_JIT_FAIL_CLOSED` remains set.

## Problem closed

The interpreter decrements `TGState.hotcount[]`, but the remaining C reset paths
still treated `GG.hotcount[]` plus the ambient `G2TG(g)` as a writable mirror.
That was not a lockless protocol:

- a runtime penalty/retry write could race a different TG through the shared GG
  bucket;
- a full `hotloop=` or JIT off-to-on reset filled only the caller/main mirror;
- a peer could continue with an old table indefinitely;
- a new TG could catch up before its legacy-list CAS while a reset handshake
  completed without ever enumerating it.

## Publication and ownership model

Two append-only 64-bit fields were added at the end of `TGState`, preserving all
previous VM, FFI and JIT-event offsets:

- `hotcount_reset_word`: authoritative only in `g->main_tg`. Its low 16 bits are
  the exact `HotCount` fill value and its high 48 bits are a non-wrapping reset
  generation. One acquire-release 64-bit CAS publishes the pair atomically.
- `hotcount_applied_generation`: per TG. A release store is allowed only after
  all 64 owner-private buckets have been filled from one stable desired word.

Generation exhaustion aborts instead of wrapping onto an old applied value.
If the desired word changes during a fill, the filler repeats the whole table.

`LJ_GC2_HS_RESET_HOTCOUNT` (`0x00002000`) is a counted safepoint action over the
mandatory legacy TG list. Stable-registry completeness is deliberately not
assumed. The action is applied before the TG's `hs_pending` slot is released;
`hs_epoch_ack` is not used as completion evidence because that epoch is claimed
before actions run.

An ordinary acknowledgement can fill only its exact TLS TG. A foreign leader
can fill a peer only on the existing consumed-poll `native_parked` path. A
foreign TG-only acknowledgement without either authority requeues the request
and cannot reduce `hs_pending` to zero.

## Attachment closure

Private TG initialization fills the currently published desired generation.
Attach then rechecks after the successful legacy-list CAS. The publisher fences
after its desired-word CAS and the attaching owner fences after its list CAS.
This is the two-sided closure:

1. if reset publication is observed by the post-CAS recheck, the owner fills it;
2. otherwise the reset handshake must observe the newly linked TG and signal it.

The deterministic fixture pauses an attaching OS thread after pre-list catch-up,
publishes and completes a reset while that TG is unenumerable, then releases the
CAS. The post-CAS recheck is what advances the new TG to the completed
generation.

## Dispatch and runtime bucket changes

JIT off-to-on no longer resets buckets while holding `DISPMODE_UPDATE`. It now:

1. publishes the completed global dispatch template/mode;
2. publishes a new hotcount generation;
3. runs one combined `REDISPATCH|RESET_HOTCOUNT` handshake.

An asynchronous profile update refuses to claim an unrelated JIT-bit transition
because it cannot run this counted boundary.

`hotcount_setg()` was removed. Runtime hotloop, retry, penalty and call-unroll
writes use the exact `lua_State`, its direct `tg_hint`, current-L publication and
state-owner certificate. They additionally require `G2TG(g)` to resolve to that
same TG, which proves the executing physical actor owns the target before the
plain write. A raw-TLS equality is deliberately not required because the
actor-validated universe fallback is the supported nested-universe/main-TG
path. Penalty and call-unroll randomization also use that exact TG's PRNG. No
runtime source writes `GG.hotcount[]`; the GG array remains layout/bootstrap
storage.

Prototype/function enable, disable and flush behavior is unchanged: prototype
bytecode re-enable and scoped trace flushing retain their existing paths. Only a
whole-engine off-to-on transition performs the combined full reset.

## Validation

The new `t-jit-hotcount-generation.c` fixture covers:

- initial and repeated full resets while sentinel values prove GG is untouched;
- two bytecode addresses which collide in the same hashed bucket;
- isolation of that collision between two live TGs;
- refusal of an uncertified foreign fill;
- certified remote fill of a native-parked peer;
- the real two-TG engine off-to-on `REDISPATCH|RESET_HOTCOUNT` transition;
- function-scoped off/on/flush without a full reset generation;
- `RESET_HOTCOUNT|STOPREQ` composition and zero `hs_pending` completion;
- the paused pre-list-catch-up/list-CAS attachment race described above.

Observed results on this macOS ARM64 host:

- native ARM64 experimental/assert helper fixture: pass;
- `tools/ci/jit_hotcount_generation_contract.sh`: pass;
- full `tools/ci/arm64_jit_fail_closed_gate.sh`: pass, with zero traces;
- native ARM64 `LUAJIT_DISABLE_JIT` build and numeric smoke: pass;
- thin x86_64 assert/helper build under Rosetta: focused fixture pass;
- x86_64 JIT numeric smoke: pass;
- x86_64 stock suite: 509 passed;
- x86_64 existing `t-safepoint-handshake.c`: pass.

## Deliberate limits

This is recorder preparation, not recorder admission. ARM64 hotloop callbacks
still return through the fail-closed gate, and this checkpoint constructs or
executes no ARM64 trace. The branch already has unreachable TG-local XPOLL
lowering and native-exit scaffolding, but neither is evidence of an executable
trace path. Side/stitch admission, recorder polling, safe IR admission, full
snapshot unwind and end-to-end generated-code execution remain separate gates.
The 32-bit x86 VM still addresses `GG.hotcount`; the lockless
supported/preserved backend in this fork is x86_64, and this work does not claim
a 32-bit x86 multithreading port.

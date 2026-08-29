# Windows GC2 admitted thread-cell migration (2026-07-18)

This note records a post-b1.2.0 implementation divergence and its evidence. It
does not modify the immutable plan.

## Why the b1.2.0 backend was temporary

b1.2.0 changed five GC2 ownership fields from accidental process globals to
MinGW `__thread` storage. That repaired cross-thread corruption, but GCC's
Windows emulated TLS calls `__emutls_get_address` and can allocate and lock on
first access. Those accesses occur in SMR, sweep/IDLE reclamation, and JIT body
validation, so emutls was never an acceptable final nonblocking backend.

The five fields are capabilities rather than caches:

- `reclaim_g` proves that this exact OS thread won exclusive reclamation;
- `idle_transition_gate_g` lends a phase controller's already-closed JIT gate
  only to its same-thread IDLE callback;
- `idle_reclaim_gate_owned` distinguishes an owned close from that borrowed
  close; and
- `{smr_reader_g, smr_reader_depth}` represents exactly one process-visible
  reader count for nested reads of one universe.

Sharing any of them could authorize reclamation behind another OS thread.

## Landed storage model

Windows now embeds one `LJThrGC2TLS` record in the existing `LJThrTGCell` next
to the tagged TG binding. `lj_thr_tg_tls_init()` admits and zeroes the complete
cell before publication. TG install/swap/clear never reset the GC2 record: a
nested cross-universe callback may retain the outer universe's reader identity
while the inner universe uses ordinary counted reads.

Each cell is one 64-byte cache line and is allocated with 64-byte alignment.
This avoids false sharing of the frequently updated nesting depth between two
OS threads. Published cells remain process-lifetime allocations under the
existing Windows TG lifecycle; a cell which fails publication is released with
the matching aligned free.

`lj_thr_gc2_tls_current()` is deliberately lookup-only:

- it reads the already-published process key and calls `TlsGetValue` exactly
  once;
- it never calls `InitOnce`, `TlsAlloc`, `TlsSetValue`, an allocator, or an
  admission helper; and
- it saves/restores Win32 `LastError`, because an SMR lookup may occur between
  an FFI call/callback body and the error-state snapshot.

The LastError save/restore is correctness-critical. The older tagged-TG getter
retains its documented `TlsGetValue` clobber; only the ubiquitous GC2 lookup is
transparent.

POSIX uses the same logical record in native compiler TLS. This consolidates
the five fields without changing their thread-local semantics.

## Missing-cell behavior

A raw foreign/test OS thread can reach an ordinary SMR read before Lua
admission. A missing cell therefore does not abort or allocate:

- each nested ordinary SMR read takes its own process-visible reader count;
- each leave drops exactly one count; and
- exclusive SWEEP/IDLE reclaimer entry fails closed before changing global SMR
  state.

The mandatory IDLE phase-close callback is different. Its callers have already
published IDLE and do not propagate a zero handshake result, so a missing or
occupied transition capability is a fail-stop invariant violation. Continuing
would leave a half-published phase transition.

Runtime-owned GC workers, `threading.spawn` workers, foreign attach, and main
state creation admit a cell before attach/native/VM/GC work. A normal thread
must exit with all five fields zero. Forced `TerminateThread` remains outside
the supported lifecycle because it runs no cleanup; an abnormal exit can strand
reader/global-gate state only in the conservative fail-closed direction.

## Mechanical and runtime gates

`tools/ci/windows_gc2_tls_gate.sh` now examines both `lj_gc2.o` and `lj_thr.o`.
It rejects the five legacy symbols, all emutls artifacts, GC dependencies on
lazy admission/TLS publication, a missing accessor import/definition, and an
accessor body with anything other than exactly one `TlsGetValue` lookup plus
error-state preservation.

`tests/t-windows-gc2-cell.c` is linked directly with the production Windows
objects and runs in native Windows CI and the Wine release build. It proves:

- a raw unadmitted thread retains a NULL cell and uses two independent counted
  reads, including a staged partial leave;
- two admitted OS threads have distinct records;
- same-universe nested reads contribute one global count per thread;
- nested different-universe reads remain fully counted;
- staged leaves preserve the outer count until final scope exit;
- every record is clear after join; and
- lookup/read/leave preserve Win32 `LastError` immediately across each GC2
  operation.

The existing `t-tg-tls-binding` fixture additionally retains allocation-failure
injection across a raw missing-cell SMR read, proving that the read did not
perform lazy admission.

Evidence collected on 2026-07-18:

- Linux `m4_tg_tls_binding` passed;
- Linux GC2 paranoia, full stock interpreted/JIT runs, worker scheduler, and
  JIT-token suites passed;
- MinGW production and helper builds completed;
- the object gate passed;
- the helper-linked exact TG fixture passed under Wine; and
- a full UCRT Windows b1.2.1 artifact preflight passed the production cell
  fixture, installed-binary smoke, and archive smoke under Wine.

## Remaining lifecycle debt

This removes blocking behavior from GC2/JIT hot lookup, not from first Windows
thread admission. `InitOnce`, `TlsAlloc`, aligned allocation, and `TlsSetValue`
are still cold admission operations. The final lifecycle should preallocate
runtime-owned cells in the controller and provide a bounded foreign-thread
admission backend (or a separately validated direct TLS mechanism). Until then,
no code may call admission from an SMR, reclaimer, VM, JIT, or signal hot path.

The process-lifetime cell policy also deliberately trades a small per-thread
allocation for safe reuse across sequential Lua universes. Joined-controller
reclamation or DLL-thread-detach handoff can be added only after it has an
allocation-free abnormal-exit protocol and cannot race a late TLS lookup.

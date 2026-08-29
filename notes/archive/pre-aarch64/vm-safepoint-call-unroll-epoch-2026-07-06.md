# VM safepoint call-unroll epoch assertion

`m3_vm_safepoint` had a call-unroll recorder case that asserted
`g->gc2.hs_epoch` did not change while `load_recorder_call_unroll_flush()` ran.
That assertion was too broad for the current GC2 runtime.

GDB showed the epoch increments came from recorder-side GC steps finishing GC2
sweep boundaries:

- `lj_gc2_sweep_prepare_bridge_boundary()`
- `lj_gc2_sweep_to_idle()`

Both paths legitimately call `lj_safepoint_handshake()`.  The trace cleanup path
under test still uses `lj_trace_flush_unlink()` or
`lj_trace_flush_unlink_retire_return()` from `check_call_unroll()` instead of the
scoped JIT handshake used by public `jit.flush(1)`.

The test now checks the intended invariant directly: the recorder leaves no
pending safepoint work behind, no additional scoped JIT slots are retired, and
no traces remain in the scoped-flushing state.  It no longer treats unrelated
GC2 sweep handshakes as a failure.

The recursive hotcall poll test has the same shape.  It publishes a manual
enable-barrier poll before entering a recorder-heavy recursive call.  By the
time the Lua callback observes the state, recorder-side GC may already have
advanced to a later sweep handshake such as weak-to-sweep.  That still satisfies
the VM safepoint contract if the original epoch was consumed, the current epoch
is acknowledged, and no `poll`/`reqmask` residue remains.  Exact barrier mirror
checks stay on the simple interpreter cases where no later GC2 phase handshake
is expected.

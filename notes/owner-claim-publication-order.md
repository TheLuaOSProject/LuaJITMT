# Owner claim cleanup

Several owner/state claim paths had throwable publication, stack-growth, or
buffer-growth work after a claim became visible. The permanent fix is to either
perform the throwable work before publication when the state is still private, or
to wrap the post-claim work and clear/drop the claim before rethrow/reporting.

- `threading_spawn_core()` claimed the newly-created child `lua_State` before
  growing its stack for the copied arguments. The child state is not published
  yet, so the stack growth now happens before `lj_state_claim()`. The
  subsequent copy plus `lj_state_stack_pubtv()` publication loop is now run
  under `lj_vm_cpcall()`; if publication fails, the child stack top is restored
  and the owner claim is released before rethrow.

- `threading_worker()` could throw during TG attach catch-up or stack
  publication after claiming the child state. Worker startup now runs under a
  protected transaction and always releases the state owner, detaches any
  attached TG, leaves MT/GC accounting, marks the thread DONE, and wakes waiters.

- `threading_attach()` for TLS-less foreign callbacks could throw during
  first-MT activation or TG attach catch-up after claiming the carrier state.
  Attach setup now uses a protected transaction with explicit cleanup for TLS,
  TG fields, MT entering/live accounting, and the state owner.

- `lj_threading_detach()` no longer runs dead-TG reclamation inline from the
  foreign callback thread. Auto-attach TGs are marked `TGF_HEAP`; the ordinary
  safe `lj_tg_reclaim_dead()` leader path now finalizes and frees heap TGs
  after physical unlink.

- FFI callback slot installation claimed `ctype_cb_owner[slot]` before rooting
  the hidden carrier state and before rooting the callback function side slot,
  while `cbid` was still unpublished. The install path now keeps the stock
  publication order, but wraps both post-claim root publications in protected
  calls. On error it clears the owner claim and function slot before rethrowing.

- `gc2_call_finalizer()` claimed the callback state before finalizer stack
  growth. Stack preparation now runs through `lj_state_cpgrowstack()` while the
  claim is held; if it fails, the claim is dropped before the finalizer error is
  reported.

- `luaJIT_profile_dumpstack()` claimed the inspected state before resetting and
  growing the shared profile stack-dump buffer. The dump body now runs under
  `lj_vm_cpcall()`, drops the state claim, then propagates any error.

Coverage:

- `m7_ffi_callback_install` documents callback owner/function cleanup before
  rethrow and `cbid` publication after the protected store. Behavior coverage
  owns the observable callback installation contract; source-text checks are
  obsolete.
- `m4_threading_claim_cleanup` guards spawn, worker, attach, detach, and heap
  TG reclaim cleanup boundaries.
- `m4_threading_spawn_native` and `m4_threading_api` cover the spawn path after
  the child stack-growth reorder.
- `m8_finalizer_error_native_stdio` guards finalizer stack-prep claim cleanup.
- `m5_profile_stop_native` and `m5_profile_blocked_tg_samples` guard protected
  profile dumpstack cleanup.

# Finalizer Dispatch Requeue Ownership

`gc2_finreg_dispatch_requeue()` now owns the object-state handoff after a GC2
finalizer queue dequeue and before FINREG dispatch clears registration state:

- cdata are CAS-requeued onto the root list;
- userdata are requeued after the main thread root;
- both are made current-white and arena-marked.

Legacy `gc_dispatch_finalizer_obj()` only selects the cdata/userdata GC2
dispatch API and supplies the callback function. The M8 weak/finalizer gate
rejects reintroducing requeue/rewhite state mutation there.

This keeps callback execution on the claimed caller `lua_State`, but moves more
of the finalizer object lifecycle under the GC2 dispatch boundary.

Validation:

- `tools/ci/m8_weak.sh`
- `tools/ci/m7_ffi_finreg.sh`
- `tools/ci/m3_gc2_worker_scheduler.sh`

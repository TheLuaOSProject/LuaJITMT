JIT And Dispatch Retry Yield
============================

Three JIT/dispatch retry waits no longer sleep for a fixed 1 ms:

- `lj_dispatch_update()` contention on `DISPMODE_UPDATE`;
- recorder-token acquisition after a CAS loss;
- recorder table-template marker waits after resize forwarding or slot CAS loss.

Each path now uses a short CPU pause and `lj_thr_yield()`. The recorder-token
path still runs the fresh STOPREQ check after the native yield, preserving the
shutdown behavior that the old sleep path provided.

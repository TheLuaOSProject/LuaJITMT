# Owned service receipt audit

Completed the bounded source audit on exact `f9ec0a7217fc1a7eca61e17ab783bf32b9be1c61`. Read [RECEIPT-MAP.md](RECEIPT-MAP.md) for the call-by-call branch map and [REFACTOR.md](REFACTOR.md) for the smallest internal result contract.

The principal findings are:

- Recovery completion helpers return **zero for both real retirement and fail-closed failure**. Preserve the existing return for compatibility and expose a separate exact completion result.
- `GC2_TRAVERSE_DONE` includes **thread NEEDSCAN ownership transfer**, already-covered scans and skipped/opaque handlers. It is not a payload-completion receipt.
- Successful recovery publication can mean **new, existing, redirtied, another counted publisher, or terminal-free coverage**. SSB source commitment cannot be renamed new recovery or completed rescue.
- A quantum may commit an SSB prefix, then defer; or retire a recovery count while requeueing its payload elsewhere. The receipt needs independent source, payload, successor and refusal facts. Existing zero-on-defer `progress` can remain unchanged.
- A one-file internal side-result refactor can expose these facts while keeping public step results, queue ownership, native entry, work-class scheduling and allocation-credit policy unchanged. Opaque handler outcomes must remain explicitly opaque.

No runtime source, fixture or shared file was edited. No runtime patch, build, test or debugger validation was performed. The 225-file `base/` copy is from the committed HEAD, not a candidate build. `src/lj_gc2.c` remains SHA256 `3571a3f2128114c475a730fcb35a413cffdfd27124ba2dcb9e136d9690edbea9`.

The prior foreground design at `/tmp/lj-jit-foreground-design-20260905-hwhdaa4a` remains immutable (manifest `8d9bb4a2e6757d479253b58a8a2e879d96af95ccfc730d646730c7b5864b8d3e`). This audit narrows its receipt gap; it does not resolve the separate service-rate/native-turn choice or change the preserved JIT completion failure.

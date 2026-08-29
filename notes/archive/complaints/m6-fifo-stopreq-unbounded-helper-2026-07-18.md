# M6 FIFO STOPREQ helper has an unbounded unmatched FIFO writer

Status: open harness issue; not a known runtime failure.

The `m6_jit` aggregate once stopped in
`t-safepoint-handshake.c`. GDB showed the main test thread blocked in
`pthread_join()` while `native_stopreq_thread()` was blocked indefinitely in
`open(fifo, O_WRONLY)`. The intended reader had already returned, so no future
reader could pair with that writer. The focused `m6_dispatch_redispatch` target
passed on its immediate exact retry, and the complete `m6_jit` aggregate passed
at `011a855e`. The same native safepoint fixture passed in the complete,
uninterrupted M3 scaffold run at checkpoint `16aa5cac`; that aggregate also
finished successfully and restored both strict default profiles.

The fixture currently has no bound on either the FIFO writer's `open()` or the
join, and the aggregate supplies no per-case timeout. A rare test-schedule miss
therefore looks like a runtime deadlock and can hold the suite lock forever.
Release validation used an external outer watchdog to contain this harness
failure mode. The fixture should eventually use a bounded nonblocking-open
retry (or an explicit reader/writer rendezvous), report the active STOPREQ case
label, and fail cleanly when pairing does not occur.

This complaint does not relax runtime liveness gates: a repeatable hang outside
this exact unmatched FIFO-helper stack remains release-blocking.

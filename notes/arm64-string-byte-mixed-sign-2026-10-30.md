# ARM64 mixed-sign dynamic `string.byte` ranges

## Scope

The ARM64 dynamic multi-result `string.byte` admission now handles either
normalized endpoint independently: positive first/negative last, and negative
first/positive last. It covers INT/NUM endpoint combinations, widths 2, 3, 4,
8, and 16, and first-byte positions 1 and 2. Other endpoint forms remain
closed unless they match the existing source and IR proofs.

The independent semantic and post-RA proofs bind the exact source call and
string, endpoint conversion slots, signed endpoint guards, normalized range,
checked width, and each byte displacement. The C fixture mutates endpoint
types, source slots, guards, range width, and per-byte offsets in both passes.
The ordinary probe also checks boundary inputs and native exit/reentry around
full GC with the peer TG live.

## Validation evidence

The final focused ARM64 contract passed all four ARM64/arm64e assertion and
release profiles:

`/Users/frityet/Scratch/ljmt-string-byte-mixed-sign-20261030/final-four-profile-clean.log`

The corrected ordinary probe covers 80 rows: two endpoint-sign orientations,
all four INT/NUM combinations, five widths, and two first-byte positions. Each
row had a selected native root and eight selected-root exits before and after
full GC on both architectures. Results are saved at:

- `/Users/frityet/Scratch/ljmt-string-byte-mixed-sign-20261030/ordinary/arm64/results.log`
- `/Users/frityet/Scratch/ljmt-string-byte-mixed-sign-20261030/ordinary/x86_64/results.log`

The initial draft of the probe accidentally did not use its endpoint-type
selector and therefore repeated one combination. Those runs are superseded by
the corrected 80-row probe above.

## SNEW rooting/lifetime audit

Generated ARM64 `IR_SNEW` calls `asm_snap_prep`, passes the source pointer and
length to `lj_str_new`, and sets up the GCstr result before emitting the C
call. `lj_str_new` keeps a newly allocated string private in `news` while
retrying table entry; `lj_mem_newgco_raw` allocates raw storage without linking
it into the GC object graph. Before the string becomes visible through the
intern table, `strtab_mark_before_publish` marks/preserves it for the current
GC2 phase, then the table-link CAS publishes it. Thus a wait cannot expose an
unrooted partially inserted string to the collector.

`strtab_wait` calls `lj_thr_retry_yield(L)`, which brackets the scheduler yield
with `lj_native_enter`/`lj_native_leave`. The latter may acknowledge safepoint
work, but it does not clear the trace's TG `jit_base` lease. While that lease
is published, GC2 treats the trace frame layout as authoritative, scans the
stack storage, and marks the active frame-function/prototype chain. The VM
clears `jit_base` only after trace snapshot restoration at the exit landing.
The source string remains rooted by its live stack value or by the rooted
function/prototype constant chain; the source pointer itself is nonmoving.

This source audit found no need for another rooting handoff at
`strtab_wait`. It did not add concurrency instrumentation or claim a new
stress-test result for simultaneous string-table resize and native allocation.

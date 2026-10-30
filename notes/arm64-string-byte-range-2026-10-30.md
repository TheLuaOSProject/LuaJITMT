# Explicit string.byte end arguments

An ordinary matched probe found native x86_64 roots for `byte(s,2,2)`,
`byte(s,2,4)`, `byte(s,2,nil)` and `byte(s,-2,-2)`. ARM64 initially refused
all four. The first three now have a selected native root and eight exits
for eight calls, repeated after full collection. The negative-index case
remains refused and is a separate gap.

Recorder preflight now accepts the optional third argument while retaining
the exact one-result caller window and all frame/continuation checks. Fixed
positive end arguments use the existing scalar byte-load proof. For a
requested range whose caller retains only the first byte, the optimizer can
keep `length >= 4` before loading byte two. Both independent validators now
recognize that sufficient bound, requiring `0 <= offset < guarded bound`
on the same immutable string. They still reject weaker bounds and preserve
the existing immediate-offset limit. Dynamic-offset source proofs are not
widened by this change. There are no new locks or runtime helper calls.

The native fixture compares all new cases with a separately JIT-disabled
oracle while a peer remains attached. It covers shorter strings that exit
the stronger range guard, NUL/high bytes, empty-string errors, successful
reuse after an error, collection, and identical machine code after reuse.
Private semantic and allocated copies independently reject an offset at the
guarded length, removal of the length guard, and a bound too weak for the
byte offset. Existing dynamic-index, folded and malformed-input fixtures
remain in the same complete contract.

Evidence under `/Users/frityet/Scratch/ljmt-deep-callm-integration-20261029`:

- `range-string-byte-probe.lua`, `range-byte-{arm64,x64}-before.log`: matched
  initial architecture difference, using the preserved ordinary x64 bundle.
- `range-byte-arm64-first.log`: preflight alone admits exact/nil end but
  still refuses the stronger-bound range, distinguishing the two checks.
- `range-byte-arm64-bounds.log`: all three positive/end-nil cases have native
  witnesses; the negative case remains explicitly unwitnessed.
- `range-byte-four-profile.log`: full string-byte contract passes on ARM64
  and arm64e, assertions and release, including BTI profiles; ordinary
  restoration succeeds with profile
  `eaf499e45488f951e494dcdf125f1e1f526d11ed95fe6b4b5bfbb801e6beb125`.
- `range-byte-stock-{source,bytecode}.log`: ordinary ARM64 passes all 509
  source cases and all 509 cases imported as genuine original v2 bytecode.

This is focused evidence for the range extension. The next combined gate
and full x86_64 behavior parity remain separate work.

## Review follow-up

The range-length `SUBOV`/`EQ` proof was checked separately because a dynamic
end below the start must return no values. `range-byte-dynamic-end-arm64.log`
and `range-byte-dynamic-end-x64.log` show that integer, fractional and negative
numeric ends, plus a shared dynamic start/end, remain refused on ARM64 while
the x64 roots execute. `range-byte-dynamic-end-dump.log` records the dynamic
end's unsigned length comparison, range-length subtraction and equality; its
ARM64 admission fails before assembly. The new fixed positive-end cases fold
that subtraction/equality to constants, so they do not rely on an unproven
dynamic range guard.

A parameter whose observed value is nil does compile, with its nil type
guard. The fixture now tests that specialization against later ends 1, 4 and
-2. Each call enters and exits the selected native root, agrees with the
JIT-disabled oracle (including the empty-range error), and preserves the code
for subsequent nil reuse. The updated complete four-profile contract passes
in `range-byte-nil-end-four-profile.log`, restoring ordinary ARM64 profile
`71da45282b5aa0f6500e8b34bdcb3d9f8cc59ef6f164ce25ef8427489bf4c4f4`.
This follow-up changes tests only, not production code.

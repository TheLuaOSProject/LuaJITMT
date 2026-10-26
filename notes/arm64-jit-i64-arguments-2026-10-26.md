# Fixed owned I64/U64 arguments on Darwin ARM64

## Scope and source proof

This increment extends the experimental ARM64 CALLXS admission checks to
fixed, exact-terminal-type scalar `int64_t` and `uint64_t` CData arguments.
The recorder already emits `FLOAD CDATA_INT64`; existing ARM64 call lowering
uses full-width GPRs and eight-byte fixed stack slots. No recorder, optimizer,
allocator, emitter, shared runtime, or x86 behavior change is intended.

Both admission passes independently require an owned CData source. A guarded
SLOAD must have its own exact CType-ID load and equality guard before the
payload read. A CNEW source must have a matching earlier ordinary CALLXS,
unique payload store, completed native leave/zero guard, and the existing
boxed-result snapshot/lifecycle proof. The raw terminal CType must equal the
fixed argument's ABI CType, including signedness and retained qualifiers.

The pre-native XSAVE must stage that exact CData source in the corresponding
Lua argument slot. A different same-type root, an unrooted register/spill, or
a NORESTORE slot is insufficient. Every newly admitted payload load is confined
to fixed ABI argument uses. References/XLOAD, conversions, CData PHI arguments,
and 64-bit variadic suffix carriers remain outside this certificate. Existing
scalar variadic suffix support may compose with a certified fixed prefix.

## Focused validation

The focused contract is `tools/ci/arm64_jit_i64_args_contract.sh`; the new
lifecycle and independent malformed-view fixtures are also registered in the
M7 scalar CALLXS case. The first complete ARM64 assertion run passes: 344 native call observations,
four authentic positive trace clones and 26 independently rejected mutations
in each of the semantic and post-RA passes. The older pointer admission
fixture also passes. The contract restores the ordinary ARM64 experimental
assertion runtime without test helpers. Log:
`/tmp/ljmt-i64-args-admission-second.log`, helper profile
`92338ca8d4b985bd28002cc2f393e7a36062206b999258e38881515a04dd72e9`.

The native fixtures cover two full-width signed/unsigned pattern sets, eight
GPR arguments and a ninth stack argument, real full-width spills across native
calls, typedef and exact const-terminal signatures, an ordinary boxed producer
followed by a fixed consumer, exact CData root identity, errno, forced POSTCALL
exit without replay, and a fixed I64 prefix plus promoted double/bool/string
suffix. A second positive trace passes two distinct completed CNEW results to
one nine-argument consumer. Malformed clones test source/type/signedness/width,
late guards, raw references/conversions/PHIs, exact argument slots and
NORESTORE, ninth stack carriers, and unrelated same-type boxes. A boxed
CALLM variadic suffix remains unpublished and executes correctly in the
interpreter.

The four-profile matrix and combined regression gates are still pending.

Initial fixture bring-up caught a missing header and an invalid harness call
geometry: directly entering the runner from C hits the existing postcall
recorder restriction requiring a Lua caller frame. The fixtures must use
ordinary non-tail Lua drivers, as the existing boxed-result tests do. This does
not justify weakening that separate recorder boundary. Initial fixture-only
CARG decoding and argument-slot mutations were also corrected before relying
on their negative evidence. The helpers return signed int32 checksums so this
argument test does not require the still-closed U32-to-NUM result conversion.
The runtime trace includes both preheader and loop copies of its calls; the
clone tests require those actual counts rather than assumed source-call counts.

The full ARM64 port remains incomplete. This increment does not establish
general 64-bit arithmetic, aggregate/sret calls, arbitrary call chains,
CALLM boxed producers, general traces, or full sustained concurrency parity.

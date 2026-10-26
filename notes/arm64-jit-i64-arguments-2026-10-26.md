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

## Frozen matrix and main rebuild

Frozen checkpoint `f0c4f574c53c8f26130d2d434354b9a3f021b92e` passes the
focused four-profile contract:

| Target/runtime | Profile |
| --- | --- |
| ARM64 assertion | `a88438493529bfe0d3c0f1bfd84b96a963178a07cf79a98cd0702b0ff06b408d` |
| arm64e assertion/BTI | `069cffcb92a99e5937ddc5475f84f4f121d079701248c11e063aa0eea604a1e2` |
| ARM64 release | `60165024eeae6475123c32f61e7d1a46bd4aaf58c27ae89fc2d401b749f7e6c3` |
| arm64e release/BTI | `c3b38b7d2c613d41afe6892aa2842b1b0d23d5552069d669760e506b868af0fd` |

Each profile records 344 native argument-call observations, four authentic
positive traces and 26 rejected mutations in each independent admission pass.
Existing pointer admission also passes. The log is
`/tmp/ljmt-i64-args-four-profile.log`.

The complete experimental and bootstrap gates pass at the same frozen source.
The experimental gate includes all four M7 **scalar CALLXS** profiles, with
the new fixtures alongside float/variadic/boxed-result calls, callback
lifecycle, remote flush and C++ unwind. This is not a claim that the entire
M7 umbrella ran. Bootstrap passes its 387 applicable stock cases, native
roots/GC2, signal polling, callbacks, interpreter aggregate/variadic FFI and
x64 errno unwind. Logs are `/tmp/ljmt-i64-args-experimental.log` and
`/tmp/ljmt-i64-args-bootstrap.log`.

Ordinary ARM64 assertion profile
`ccfebfe759f4e81cc988863074a27372f984b0614e9a562b85cf539b75249da5`
passes six new fixed-I64/U64 argument smoke cases, the nine earlier boxed
result cases, bitops, rooted cells/upvalues, and both 509-case stock source
and original-v2 bytecode-import corpora. Signed x64/Rosetta assertion profile
`774e9ccfb59325ab275bfdc33b7e2af554e127004dd588e044f1af970de6046c`
also passes both 509-case corpora. These are targeted compatibility regressions,
not a complete x64 umbrella or native Intel performance evidence. The matrix
verifies unchanged source and restores ordinary ARM64 assertion profile
`ebe01e60e94e4f1a6f4b73af4cd3aaf2ce92ad0d0f0e5aea780b9cca5fa436d9`.
The stage ledger is `/tmp/ljmt-i64-args-matrix.log`; related logs share that
prefix.

The main `v2.1` checkout was fast-forwarded to the same source and rebuilt
with only `LUAJIT_MT_ARM64_BOOTSTRAP`, `LUAJIT_MT_ARM64_JIT_EXPERIMENTAL`
and `LUA_USE_ASSERT`. Main profile
`3873e3b208437ef82bf3c5a58cc940ae3f2ceec41e18db490e90061e6bfad273`
passes all six argument and nine boxed-result cases, bitops/cells/upvalues,
and both 509-case corpora (`/tmp/ljmt-i64-args-main.log`). These ordinary
smokes check exact results/counts and published trace metadata; the instrumented
C fixtures above provide the native-entry observations. Code-sign validation
passes, selected test-helper symbols are absent, and the executable has no
out-of-line atomic runtime dependency.

Main artifacts built at `f0c4f574`:

- Runtime SHA-256: `45bed875e7ec444522771a7b995402563f3aac27a609598fe17aaff4fe3552fa`.
- Archive SHA-256: `564e816a1dda7ab83efdf85d58363b62351a0fd4cb8b0e7fdc1c53a595b8d1fb`.
- Recorded build-signature SHA-256: `679950fba5efbea304f527f61d599524dd4f3fcf8f9b8d95e8240bfed1e1cc56`.

Subsequent evidence-note edits do not change the tested production source.

## Remaining scope

The full ARM64 port remains incomplete. This increment does not establish
general 64-bit arithmetic, aggregate/sret calls, arbitrary call chains,
CALLM boxed producers, general traces, or full sustained concurrency parity.

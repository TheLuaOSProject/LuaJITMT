# Direct scalar CALLM chains

The experimental ARM64 FFI certificate now admits a direct nested scalar
call such as `outer(fixed, inner(args))`. The producer is an adjacent `CALL`
with a single native scalar result and the bytecode multires result count;
the consumer is `CALLM`, whose complete variable tail is exactly that result.
Independent semantic and post-register-allocation passes check the source
register/count geometry, completed producer lifecycle and final argument
identity. Only the already supported exact scalar conversions may carry the
result into the consumer. LOOP/XPOLL boundaries clear the producer state.

This changes target-local admission only. Existing emitters, native frame
publication and cleanup remain in use. The allocated-result check also accepts
FLOAT results moved from S0 into D8-D15 by the existing `ra_destreg` path;
the nested float stack-argument case requires that preserved location across
leave/enter helpers. NUM results retain their previous allocation rule.
Independent spill, live-across-call, source and snapshot checks still apply.

The consumer's entry failure may resume at its own CALLM instruction, using
the completed producer result. Its post-call exit must resume after CALLM.
CALLM remains excluded from the replayable setup allowlist: a completed
foreign call may not be replayed. General multires, tail calls, aggregate
results and other CALLM source geometries remain closed.

## Validation

The frozen regression ran from base
`949ffc3a91c8f3d8f3203f884d80775fdbe1c051`, with binary diff SHA-256
`41229a79f976b8dc6426ac59ee8c05ed3912c7adb1255dc5d66908aa9723ad29`.
All four complete M7 scalar FFI suites passed: ARM64 and arm64e/BTI, assertion
and release. Their coverage includes:

- Nested integer calls, an integer ninth stack argument, FLOAT-to-NUM calls,
  and FLOAT/FLOAT and NUM/NUM ninth stack arguments. Each long case requires
  native ACTIVE-frame observations for both calls, with exact source effects.
  The float and double cases observed 298 native producer/consumer calls out
  of 300 source iterations in each profile.
- Six forced POSTCALL cases: producer and consumer exits for integer,
  float-stack and double-stack pairs. Each performs exactly 20 source calls
  to each function. A producer exit observes 19/18 native calls; a consumer
  exit observes 19/19. Exact continuation PCs and cleared native activity,
  JIT frame and trace pins establish no replay and completed cleanup.
- Independent source-count/register, missing-producer, final-argument and
  return-snapshot mutations. NIL/CDATA/P64 producer IR mutations are rejected;
  these are malformed graph tests, not new valid void/aggregate ABI coverage.
- Allocated FLOAT result mutations to D7, D16, X19 and no location are rejected.
  The valid case checks the emitted S0-to-preserved-register move and outgoing
  single-precision stack store.
- The production fixture publishes 24 XSAVE/CALLXS pairs, including the direct
  nested `fabs(fabsf(x))` case and the earlier split-local chains.
- Existing ABI/classifier and admission tests, errno, nested/error callbacks,
  GC/STOPREQ, remote flush, integer/double/boxed-pointer lifecycle, and C++
  unwind/recovery. Both C++ register and stack fixtures observe 1,195 generated
  calls per profile and recover after collection and flush.

ARM64 stock source and original-v2 bytecode import each passed all 509 cases.
The worktree was restored to experimental ARM64 assertion configuration,
profile `13dca0c78fdb1825a8af63673912eb5edd7bb99fcc3be811814d9e98a1f2e915`.
The driver verified the same source diff after all runs. Documentation was
added afterward.

Driver: `/tmp/ljmt-callm-final-regression.sh`. Logs:

- `/tmp/ljmt-callm-final-regression.log`
- `/tmp/ljmt-callm-m7-arm64-final.log`
- `/tmp/ljmt-callm-m7-arm64e-final.log`
- `/tmp/ljmt-callm-m7-arm64-release-final.log`
- `/tmp/ljmt-callm-m7-arm64e-release-final.log`
- `/tmp/ljmt-callm-arm64-stock-source-final.log`
- `/tmp/ljmt-callm-arm64-stock-import-final.log`

Initial drafts exposed a producer source-count check error, an unsupported
SUBOV in a test setup, a too-narrow FLOAT result-location gate and an incorrect
production fixture pair count. These were corrected before the frozen run.
This is a focused checkpoint after rooted nil stores. It does not claim a new
combined experimental/bootstrap, x64, sanitizer, sustained stress or
performance run. The full ARM64 port remains incomplete.

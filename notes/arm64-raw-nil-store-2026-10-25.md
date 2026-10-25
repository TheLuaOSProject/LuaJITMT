# Shared raw nil stores must remain observable

The active-MT recorder sampled the destination before selecting a store helper.
If a raw `TSETR` sampled nil and its source was also nil, it emitted no store
and no absence guard. A later execution could therefore leave a newly inserted
key present instead of deleting it. The source table's state at recording time
cannot justify omitting a shared write.

This is reproduced on ARM64 at `4b2371e4`. A fixture changes a private function's
single `TSETV` to valid integer-key `TSETR` before MT or trace publication, then
uses the existing `mt_entering` test mechanism to select shared routing. It
warms a loop while key 1 is absent, installs `4242` with the C API, and executes
the same loop again. The baseline published one native trace, reported twelve
exits, and failed because `4242` survived. Log:
`/tmp/ljmt-raw-nil-arm64-baseline.log`.

The narrow correction aborts recording of this sampled-absent raw nil write
until a rooted deletion helper or an authoritative absence guard exists.
Ordinary metamethod-sensitive stores and non-nil stores keep their existing
paths. There is no new lock or table-generation access.

`tests/t-jit-raw-nil-store.c` checks the observable deletion after recording,
without depending on a particular future lowering. It is registered as
`m6_jit_raw_nil_store` and in all four assertion/release ARM64/arm64e profiles
of `arm64_jit_rooted_gc_contract.sh`.

The registered test passed in the native ARM64 assertion profile after the
fix. The unproven loop was rejected during recording and the inserted key was
deleted correctly. Log: `/tmp/ljmt-raw-nil-fixed.log`. The complete rooted GC
contract then passed this regression on ARM64 and arm64e/BTI in both assertion
and release builds: `/tmp/ljmt-scalar-keys-contract.log`.

The x86_64 build succeeded, but this host could not start its runtime: the
process remained in uninterruptible OS wait before executing the build-profile
script, with zero recorded CPU time. A termination request did not reap it;
the build driver was stopped and the native ARM64 development profile restored.
Thus this run supplies no x86_64 execution evidence. The source correction is
shared because the erased-write branch is shared.

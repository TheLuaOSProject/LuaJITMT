The proposed permanent fixture correction is ready for ROOT review. It is
isolated at candidate/tests/t-gc2-worker-scheduler.c with candidate.patch.
No shared source, fixture, build output, or commit was changed by this agent.

The failure diagnosis is frozen separately at
/tmp/lj-gc-scheduler-review-20260905-zuvqerrl. Its diagnosis-manifest.json hash
is 1a6c0967081e3e7e1c64df329f42fb0971dd8a9631ba6ba379b7652471b07652.
Seven matched diagnostic failures reproduce the original assertion on the
pristine control, initial admission candidate and STOP-veto candidate. Each
has one main-owner SSB entry left private while published/grey/recovery work
and both worker suffixes are empty. Five also retain the actual ignored flush
return, always zero. The original failed run did not capture its internal
state; its exact cause is not claimed as directly observed retrospectively.

The correction establishes the missing publication precondition. A local
publish_owner_ssb helper first accepts an already-empty owner suffix, otherwise
calls the existing bounded lj_gc2_flush_ssb. On a zero return it sleeps 1 ms
and retries, for at most 1000 attempts. It neither clears an SSB cursor nor
claims a worker/root token nor performs a full GC drain. The existing flush
can recycle one published buffer through its already-existing bounded path;
that was also possible in the previous one-shot call.

The async MARK subtest now asserts this publication succeeds before observing
the independent workers. Its parent/child/grandchild graph, mark calls, every
old semantic and worker-counter assertion, wait_until_marked, wait_ssb_empty,
and their original 1000 x 1 ms bounds are unchanged. This is a separate bounded
publication precondition, not an extension of the old observational timeout.
The outer process limit remains 60 seconds. The async WEAK subtest remains
unchanged to keep this patch tied to the reproduced MARK failure. No existing
equivalent helper was found under tests/lib; the exact rg query and its
expected no-match status are in existing-helper-search.json.

The candidate was compiled against all three frozen, matching helper archives
and ran the entire scheduler fixture 20 times per archive: 60/60 passed. The
three compiles passed. Flags are exactly:

    cc -std=gnu11 -O2 -g -Wall -Wextra -Werror
    -DLUA_USE_ASSERT -DLUA_USE_APICHECK -DLJ_GC2_TEST_HELPERS
    -DLJ_TRACE_TEST_HELPERS -DLJ_TAB_TEST_HELPERS -DLJ_FUNC_TEST_HELPERS
    -DLJ_UDATA_TEST_HELPERS -DLJ_STR_TEST_HELPERS

Each includes that archive's src directory and links its libluajit.a with
-Wl,-E -lm -ldl -pthread and --wrap=pthread_create/--wrap=pthread_join,
as the existing fixture requires. There is no new runtime build and no new
ASan claim. Runtime subprocesses were fresh, sequential and unpinned, with
the variants interleaved. All exact commands, source/archive/ELF hashes,
stdout/stderr, environment additions, time bounds and statuses are in
results.json and run.py. The named "candidate" runtime variant is the old
three-file admission runtime, not a new runtime source change in this package.

Both required oracles remain active, demonstrated by six intentional negative
controls (two per archive). All six negative-control compiles passed:

- refuse adds a GNU link wrapper around the existing public flush call and
  arms it only immediately before the new publication assertion. It returns
  the contract-permitted zero without touching an SSB or GC protocol field.
  All three runs record exactly 1000 refused calls and one remaining private
  GCRef, then abort at assert(publish_owner_ssb(g, tg)). The actual candidate
  helper body is unchanged in this control. Earlier fixture calls use the
  real implementation.
- false-success deliberately replaces only the new helper with a false
  success return, leaving the actual owner suffix private. All three runs
  record that one-entry suffix and then abort at the original unchanged
  assert(wait_ssb_empty(g)). The grandchild mark wait has already succeeded.
  This rejects claiming publication without actually exposing the work.

No expected-negative result is counted as a positive fixture pass. The exact
controls and their patches are under refuse and false-success; logs and
commands are in negative-results.json and negative-run.py. An earlier
unbuilt refusal sketch is retained as refuse/simulated-refusal-unbuilt.c;
it was never compiled or executed and is not evidence for a test outcome.

Verification checks that reversing only the new helper and its MARK call site
reconstructs the exact original fixture, and that both observational wait
functions remain byte-identical. All three runtime source trees still match
their original 807-input archive apart from the known three/six patch files.
The archive hashes remain unchanged. Shared HEAD's fixture still matches the
frozen original at the final read-only observation recorded in verification.json.

This patch repairs a test publication precondition. It does not establish
full nonblocking GC, repair the separate two-worker SWEEP completion bound,
fix STOP/RESTART threshold publication, or enable concurrent string-body
reclamation. Those production issues remain independently tracked.

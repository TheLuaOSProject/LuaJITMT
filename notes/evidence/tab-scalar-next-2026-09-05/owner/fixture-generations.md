All listed sources and their compile/runtime records remain byte-exact.
No failed generation was overwritten and no runtime implementation changed.

1. t-tab-scalar-next-authority.c: compile failed because the fixture used the
   nonexistent TABARRAY_RETIRED spelling. There were no runtime processes.
2. t-tab-scalar-next-authority-v2.c: corrected TABARRAY_FLAG_RETIRING and made
   known raw arena bookkeeping independent of a potentially protected current
   vector address. All eleven modes failed the fixture's whole-word
   remote_active == 0 assertion.
3. t-tab-scalar-next-authority-v3-diagnostic.c: one retained diagnostic failure.
   The first real successful primitive call had phase IDLE, status FOUND,
   arena remote_active BEFORE 0xa000000000000000 and AFTER the identical word.
   Those are existing CLOSED|PENDING state bits, with zero publisher count.
   The failed zero-word oracle was incorrect after real full GC. This diagnosis
   did not infer or conceal a leaked reader.
4. t-tab-scalar-next-authority-v4.c: requires the exact before/after publisher
   word and zero low-bit count, preserving the closed/pending state. Five
   opaque/protected/bounds/stage modes passed. Six other modes failed because
   the fixture read setintV output with intV even though these builds use the
   single-number representation. These are preserved assertion failures.
5. t-tab-scalar-next-authority-v5.c: a mechanical substring replacement also
   changed setintV into nonexistent setnumberVnum. Compile failed; no runtime.
6. t-tab-scalar-next-authority-v6.c: word-boundary replacement changes only
   intV output readers to representation-independent numberVnum. The exact
   scalar, cursor, lifetime, alias, unchanged-output and cleanup requirements
   remain. Eleven modes pass in assert and the same eleven in ASan.

Progress, stack-retry and supplemental lifetime fixture first generations
compiled and passed their final intended positive cases without revisions.
Progress's C API mode deliberately retains an actual failing upstream path;
its alarm is recorded as failure, not used as a pass oracle.

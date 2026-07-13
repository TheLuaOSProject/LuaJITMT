# Amalgamated test-helper build cannot link

Status: open build-mode issue; split static/shared test-helper builds work and
the production amalgamated build is unaffected.

While validating the packed sweep classifier, this command was tried:

```sh
make -C src clean
make -C src -j2 amalg \
  XCFLAGS='-Werror -DLJ_GC2_TEST_HELPERS' TARGET_STRIP=:
```

The all-warning-error compile first encounters the repository's existing
amalgamation-wide unused-function warnings and GCC null-region
`-Wstringop-overflow` false positives. Retrying with only those two warning
families suppressed successfully compiles both `ljamalg.o` and
`ljamalg_dyn.o`, but the final executable link fails:

```text
lj_BC_TNEW: undefined reference to `lj_gc_test_root_pending_loaded_vm'
```

The x64 VM test-helper path emits an external reference, while the helper's
definition in the amalgamated translation unit is not available to satisfy
that reference. This is not specific to the new classifier helpers; the
missing symbol is an existing root-publication test hook.

The focused suites already use a clean split build with
`LJ_GC2_TEST_HELPERS`, which passes under unsuppressed `-Werror`. A harness
fix should either make the VM-referenced test hooks externally linkable in
amalgamated test builds or explicitly declare the amalgamated/helper
combination unsupported and avoid presenting it as a valid strict target.

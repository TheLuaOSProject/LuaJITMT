# `print()` numeric basemt acquire read

## Context

`lib_base.c:print()` uses a shortcut for number-to-string conversion when the
active `tostring` is the builtin fast function and no numeric basemetatable is
installed. The basemetatable lives in `g->gcroot[GCROOT_BASEMT + ~LJ_TNUMX]`,
which is release-published through `setgcrefroot()`.

The shortcut check previously used `gcrefu()` to test the raw root bits.

## Change

The shortcut now checks the root with `gcref_acq(basemt_it(...))`.

## Scope

This is a single C-side gcroot read conversion. The remaining `gcrefu()` users
are either pointer hashing/masking helpers, lightuserdata/frame pointer
decoding, architecture backends, or snapshot-local pointer copies.

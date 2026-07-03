# String table hash-ref acquire helpers

Date: 2026-06-20

## Problem

The string table already had `lj_str_ref_load_acq()` and bucket traversal used
acquire loads internally, but the public `lj_str_hashhead()`,
`lj_str_hashflags()`, and `lj_str_hashsecondary()` macros still expanded
through raw `gcrefu()`.

Those macros are used by focused fixtures today, but they describe the shared
bucket metadata contract. Leaving them raw made it easy for a later reader to
reintroduce a plain GCRef load while inspecting the hash-link low bits.

## Fix

`lj_str_hashhead()`, `lj_str_hashflags()`, and `lj_str_hashsecondary()` now
route through acquire helpers. The prep fixture's direct bucket snapshot uses
`lj_str_ref_load_acq()` as well, so marker-bit checks and head decoding observe
the same published bucket word.

## Coverage

`m5_strtab_cas` owns the observable string-table coverage through the focused
CAS, preparation, and rehash fixtures. The acquire-load requirement is
documented here and beside the helper surface instead of being enforced by a
source-text grep. This intentionally does not cover the unrelated generic table
hash macros in `lj_tab.h`.

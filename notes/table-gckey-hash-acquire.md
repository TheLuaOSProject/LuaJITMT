# Table GC-key hash acquire helper

Date: 2026-06-20

## Problem

The table hash-vector reader work already moved core table accessors onto
explicit `(node, hmask)` snapshots, but `hashgcref_node()` and `hashgcref()`
still decoded collectable-key pointer bits with raw `gcrefu()`.

Most callers pass local key snapshots, but the helper names define the table
GC-key hashing contract. Keeping the raw spelling there made it easy for future
hash probes to bypass the acquire GCRef vocabulary.

## Fix

Added `gcrefu_acq()` beside `gcref_acq()` and routed table GC-key hash helpers
through it. The focused table-node publication fixture now includes a
table-as-key rawget/rawset smoke so the generic collectable-key hash path is
compiled and exercised.

## Coverage

`tools/ci/m5_tab_node_publish.sh` now rejects `gcrefu()` inside the
`hashgcref*` macro bodies. This keeps raw unsigned GCRef reads out of table
GC-key hashing while leaving construction-owned and non-x64 backend raw users
for their separate audits.

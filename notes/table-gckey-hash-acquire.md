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

`m5_tab_node_publish` owns the observable collectable-key hash path by building
and exercising table-as-key rawget/rawset behavior. The acquire-load
requirement for `hashgcref*` is documented here and beside the helper surface
instead of being enforced by source-text matching. Construction-owned and
non-x64 backend raw users remain separate audits.

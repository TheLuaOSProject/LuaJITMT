## GC2 SMR reclaim counter helpers

The GC2 SMR drain path updates `smr_reclaim_runs` and `smr_reclaimed` after it
frees retired string, table, ctype, mcode, or trace objects. This slice routes
those counters through `gc2_smr_*()` helpers in `lj_obj.h`.

Runtime initialization and increments now use the helper family, and the
string-table CAS fixture reads the counters through acquire helpers. The M5
string-table guard rejects raw access to the SMR reclaim counters in the
runtime and focused fixture.

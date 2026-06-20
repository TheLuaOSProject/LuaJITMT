# Metamethod name root acquire reads

## Context

Metamethod name strings are stored in `g->gcroot[GCROOT_MMNAME + mm]` during
`lj_meta_init()` via `setgcrefroot()`, which is a release store. Runtime C
paths then use `mmname_str(g, mm)` to look up metamethod slots in Lua tables,
ctype metatables, recorder guards, and debug helpers.

Before this slice, `mmname_str()` used the plain `strref()` helper.

## Change

`lj_obj.h` now provides:

```c
#define strref_acq(r) (&gcref_acq((r))->str)
```

and `mmname_str()` uses it for `GCROOT_MMNAME` reads.

## Scope

This does not change the generic `strref()` macro. The acquire conversion is
limited to the gcroot-backed metamethod-name roots, matching the long-tail
gcroot publication/read contract in plan section 6.8.

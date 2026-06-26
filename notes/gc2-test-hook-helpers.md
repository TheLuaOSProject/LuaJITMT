## GC2 test-hook helpers

Assertion/paranoia-only GC2 test hooks now use helper accessors instead of raw
`g->gc2` field operations. This covers FINREG cdata preclaim failure/publish
pause controls and finalizer drain pause controls.

Runtime setup, pause/release test APIs, and focused C fixtures use the helper
families. The M7 FINREG and M8 weak/finalizer guards reject raw production C
access to those test-hook fields.

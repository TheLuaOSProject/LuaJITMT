# FNEW arena-owned body markers

Fresh Lua closures and closed local-cell upvalues allocated by the FNEW active
black bump paths do not need to be published to the legacy root spine. Their
semantic reachability is through the normal Lua graph and publication barriers;
their body lifetime is owned by the GC2 arena bitmap.

Those bodies use type-local marker bits while they are not on the root spine:
the high bit of Lua-function `nupvalues` and the high bit of upvalue
`immutable`. Semantic reads mask those bits, so Lua-visible upvalue counts and
immutability stay unchanged. C closures keep the full `nupvalues` byte as their
stock API-visible count. The marker is deliberately narrow:

* only fresh `~LJ_TFUNC` Lua closures and closed `~LJ_TUPVAL` local-cell bodies
  may use it;
* tables, protos, threads, strings, cdata, userdata, open upvalues, and finalizer
  paths must stay on their existing publication/ownership routes;
* arena-body sweep may destruct an unmarked marked FNEW body, but `nextgc`
  remains reserved for root and open-upvalue chains.

The legacy mark bridge still uses the root publication path. Removing that
fallback also requires bridge preservation and legacy color normalization for
non-spine arena-owned bodies.

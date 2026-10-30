# ARM64 `string.sub` explicit negative end

## Scope

ARM64 now admits the bounded fixed-call forms `string.sub(s, -first, -last)` where both indexes are negative KSHORT literals, `first` is in the signed 16-bit range, and `last` is in that range. The existing omitted-end form remains `last = -1`. The accepted trace must contain the same guarded string load, its string-length FLOAD, the normalized start `len + first`, a nonnegative-start guard, the normalized exclusive end (`len` for `last = -1`, otherwise `len + last + 1`), guarded `SUBOV(end, start)`, a nonnegative-width guard, and the matching `STRREF`/`SNEW` pair. Semantic and post-RA passes reconstruct the source and IR relationships independently. The allocator and emitter are unchanged.

Dynamic endpoints, unrelated length values, nonliteral endpoints, and source/control-flow shapes that do not meet the bounded grammar remain interpreted. The range proof follows the VM's inclusive end-index semantics: `last < first` yields an empty result through the guarded width path.

## Evidence

The exact d87 ordinary baseline had no ARM64 loop root for explicit negative ends `-2`, `-3`, or `-4`; the matching x86_64 baseline published roots. Current ordinary builds were made from this branch's candidate source with no test-helper flags:

- Source: `6d5a8575c95704154162017a9da70f82546dd534f59ce3354131569fa39d70d3`
- ARM64 flags: `-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT`
- x86_64 flags: `-DLUA_USE_ASSERT`
- Both architectures published and executed selected JLOOP/JFORL roots for six endpoint pairs (`-1/-1`, `-4/-2`, `-4/-3`, `-4/-4`, `-32768/-32768`, `-32768/-32767`) in both loop forms. Each row checked the interpreter oracle, selected-root exits after peer GC, boundary lengths, cold type/error behavior, SNEW presence, and stable machine-code identity.
- Candidate binary hashes, build logs, modules, and per-architecture output are preserved under `/Users/frityet/Scratch/ljmt-negative-first-substring-20261030/negative-end-candidate-ordinary/`.
- Four-profile assertion/release ARM64 and arm64e contract passed. It covers native positive paths, peer/GC behavior, boundaries, and independent semantic and post-RA mutations for source endpoint, same-string length/end calculation, lower bound, SUBOV operands, snapshot/footer, and allocation/register shape. Log: `/Users/frityet/Scratch/ljmt-negative-first-substring-20261030/negative-end-four-profile.log`.

The earlier d87 baseline binaries and logs are preserved under `/Users/frityet/Scratch/ljmt-negative-first-substring-20261030/d87-ordinary/`; the matched probe and pre-change IR dump are in the parent evidence directory.

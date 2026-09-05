The cache/lifecycle repair passes 444 independent intended-positive runtime processes on ROOT's frozen v2 Linux/x64 normal, assertion, and ASan builds. No production source, shared build tree, or prior completed package was changed.

- `review.md`: unchanged main fixture, 324 processes, including actual native hit→close and 72 GC-enabled lifetime cases.
- `supplement-review.md`: additive 120 processes for NaN/±infinity and original-cache storage growth, before MT and during active MT, with old root/installed-side execution.
- `handoff-summary.json`: aggregate final results and exact source-matched baseline controls.
- `final-source-identity.json` / `exact-baseline-identity.json`: all 224 production inputs, frozen runtime binary hashes, and exact baseline commit.
- `artifact-manifest.json`: text and hash-only binary/archive classification for this package.

The main fixture remains `t-clib-cache-authority.lua`, SHA-256 f43a25e242d40469a606cd48c553ea7b59ee678c4242b0e72478931ede6cefed. `t-clib-cache-between-close.c` remains eb2fcc06d171c08ddade7edff84613a19c8006f287b433045f4f487ee5babab4. The supplement is a separate Lua file and metadata observation module. Exact compilation commands, matching helper flags, dependency hashes, runtime commands, environments, exit codes, stdout/stderr, and output hashes are in the corresponding result JSON files.

The exact 5c455f20 baseline passes all 60 interpreter controls and fails 58 of 60 native semantic controls; two ignored-setfenv native controls pass. Earlier 597b8705 controls and two explicit native-witness negative controls remain preserved separately. No new runtime blocker was found. Full-GC lifetime checks do not establish simultaneous collector execution inside the lookup helper, and this package makes no SMR-refusal timing or performance claim.

Reproduction argument forms are:

    luajit -jon t-clib-cache-authority.lua KIND MODE root|side ./authority.so helper [gc]
    ./final-candidate-between-close t-clib-cache-authority.lua read|write between-close root|side ./authority.so [gc]
    luajit -jon t-clib-cache-supplement.lua KIND MODE root|side ./authority.so helper|no-helper gc mt|pre-mt ./geometry-candidate.so

Use helper for the current active-MT build and no-helper for pre-MT or the old baseline. The C wrapper has JIT enabled internally and always requires the real hit helper. Build each wrapper/module with the exact matching runtime header/helper flags; the assertion/ASan observer locally includes the unchanged assertion formatter because that symbol is hidden in the runtime. `run-final.py` and `run-supplement.py` contain the complete bounded matrices. Their result-file guards prevent overwriting frozen evidence.

ROOT owns archive integration, canonical registration, release readiness, and the separate performance results. Keep this package immutable after handoff.

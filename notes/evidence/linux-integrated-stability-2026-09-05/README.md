# Combined Linux functional evidence

See notes/linux-integrated-stability-2026-09-05.md for the scope and limitations.
metadata.json records the d680421c base plus exact final arena/scalar overlays
and test hashes. build-*.json and build-*.log record both immutable builds.
validation-results.json contains all 30 passing processes, compile commands,
finite run bounds, outputs, and exits. Raw text outputs are also preserved.
The archive and executables remain in the original /tmp directory; no binary
artifact is committed here. The measured JIT resize fixture precedes only the
final event-loss comment and assertion wording, verified separately.

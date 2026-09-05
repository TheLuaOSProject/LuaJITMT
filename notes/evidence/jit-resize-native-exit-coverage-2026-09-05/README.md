# JIT resize observer evidence

The main note is notes/jit-resize-native-exit-coverage-2026-09-05.md.
results.json retains the original trace-count failure. Reproduction and
diagnostic records are historical. native-exit-validation.json describes the
rejected per-worker global attachment design and includes three positive
failures. native-exit-v2-validation.json contains all 80 positive passes and
four intended negative failures of the controller design. final-wording-results.json
contains one positive and four intended negative results after only final
comments/assertion wording changed. An intended negative failure is not a
positive runtime pass; expected_result labels that distinction.

Replay each base patch against d680421c tests/t-tab-resize-stress.lua. Negative
and final-wording patches apply to the corresponding preceding version named
in their filename. fixture-identities.json preserves full source identities;
patches avoid duplicating eight complete Lua files. Drivers preserve original
absolute paths and raw results. The initial runtime metadata verifies all
production files match d680421c and identifies the actual executable.

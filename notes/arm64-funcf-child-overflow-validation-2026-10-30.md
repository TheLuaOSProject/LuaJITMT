# FUNCF child overflow validation follow-up

At source commit `3e4b91ea5`, the FUNCF child-overflow assertion contract
passed after `open_kgc_function_ref()` was changed to walk constant heads and
skip two-word payloads. This is a test-helper boundary fix; production source
is unchanged. The earlier release contract passed both ARM64 and arm64e
profiles before this test-only change. The corrected assertion run and prior
release run are recorded in:

- `/Users/frityet/Scratch/ljmt-funcf-child-overflow-20261030/funcf-record-assert-boundary-walk.log`
- `/Users/frityet/Scratch/ljmt-funcf-child-overflow-20261030/funcf-record-release.log`

The ordinary ARM64 runtime was rebuilt without test helpers using bootstrap,
experimental JIT, and assertions. Its executable SHA-256 is
`e05c6803294fcf1ec5e9e66075b688b1046897490f9a01a899dc6d372826e91d`.
Both the stock source suite and genuine original-v2 bytecode import suite pass
all 509 cases. An observer-free mixed CALLM probe with a live threading peer
reports the selected root and eight identity exits before and after full GC
for each of its six numeric producer cases. Logs:

- `/Users/frityet/Scratch/ljmt-funcf-child-overflow-20261030/arm64-stock-source.log`
- `/Users/frityet/Scratch/ljmt-funcf-child-overflow-20261030/arm64-stock-bytecode.log`
- `/Users/frityet/Scratch/ljmt-funcf-child-overflow-20261030/mixed-callm-ordinary-arm64.log`

These checks validate the bounded source and this ordinary runtime profile;
they do not establish full x86_64 parity.

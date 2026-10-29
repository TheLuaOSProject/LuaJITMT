# Nested-side integration with current ARM64 features

The nested side checkpoint (`7c780d745`) and its root reentry repair
(`05bc9923e`) are integrated on the stack/tail/ITERL branch. The FORL proof merge
preserves both the existing direct constant-stop proof and the new inherited
root range proof as separate alternatives; mixed evidence is refused. The
boundary snapshot helpers from both implementations remain present.

Review added ancestry checks before recursive certificate traversal, bounds
before IR_BASE reads, and a private copied self-parent certificate rejection.
The current root-entry shell contract now checks the flattened-list walk's
immediate-parent dispatch and complete count/head rechecks. Its first attempt
stopped before runtime on a stale `current.nextside` source-text assertion;
that assertion is updated to the new list-node accessor.

Focused root-entry, first-side lifecycle, and FORL boundary validation is
pending at `/Users/frityet/Scratch/ljmt-nested-reentry-integration-20261029/`.
The separate stack/tail full gate is still running on unchanged production
`c41600c86` and cannot establish this newer nested integration by itself.

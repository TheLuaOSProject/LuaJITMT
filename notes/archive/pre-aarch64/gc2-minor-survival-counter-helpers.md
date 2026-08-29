## GC2 minor survival counter helpers

Minor survival telemetry compares live bytes before and after minor-identity
sweeps and may request a major cycle when survival exceeds the configured
threshold. This slice routes `minor_survival_base_live`,
`minor_survival_bytes`, and `minor_survival_major_requests` through helper
accessors in `lj_obj.h`.

Runtime initialization, policy updates, stats export, and the focused
allocation-account fixture now use the helper surface. The M6 allocation-account
notes document why raw production C access to these minor-survival telemetry fields.
The policy updater itself is private to `lj_gc2.c`; C fixtures use
`lj_gc2_test_update_minor_survival_policy()` for direct threshold probes.

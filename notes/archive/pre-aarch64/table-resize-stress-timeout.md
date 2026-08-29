# Table resize stress timeout

Amplified `m5_tab_resize_stress` settings can exceed the suite default timeout
without exposing a runtime failure. On 2026-07-02 the normal suite wrapper hit
its fixed `30s` timeout with:

```
LJ_M5_TAB_RESIZE_STRESS_REPS=1536
LJ_M5_TAB_RESIZE_STRESS_THREADS=6
LJ_M5_TAB_RESIZE_STRESS_JIT_REPS=4400
LJ_M5_TAB_RESIZE_STRESS_JIT_READ_REPS=4400
LJ_M5_TAB_RESIZE_STRESS_TRAVERSAL_ROUNDS=384
LJ_M5_TAB_RESIZE_STRESS_FIN_OBJECTS=384
LJ_M5_TAB_RESIZE_STRESS_KEY_OBJECTS=384
```

Running the same VM/script directly with a `180s` process timeout completed
successfully:

```
t-tab-resize-stress OK: 6 writers, 1536 resize rounds
```

The suite timeout is now controlled by `LJ_M5_TAB_RESIZE_STRESS_TIMEOUT`, with
the default still `30s` for normal CI. Use a larger value for local or release
stress amplification instead of bypassing the suite harness.

Verified after the harness change with:

```
LJ_M5_TAB_RESIZE_STRESS_TIMEOUT=180s
LJ_M5_TAB_RESIZE_STRESS_REPS=1536
LJ_M5_TAB_RESIZE_STRESS_THREADS=6
LJ_M5_TAB_RESIZE_STRESS_JIT_REPS=4400
LJ_M5_TAB_RESIZE_STRESS_JIT_READ_REPS=4400
LJ_M5_TAB_RESIZE_STRESS_TRAVERSAL_ROUNDS=384
LJ_M5_TAB_RESIZE_STRESS_FIN_OBJECTS=384
LJ_M5_TAB_RESIZE_STRESS_KEY_OBJECTS=384
tools/ci/lua_test.sh m5_tab_resize_stress
```

Result:

```
t-tab-resize-stress OK: 6 writers, 1536 resize rounds
ok m5_tab_resize_stress
```

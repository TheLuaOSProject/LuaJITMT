GC frame-header tag validation, 2026-07-07:

- `m5_tab_resize_jit_stress` exposed a crash while GC scanned a worker that was
  recording/tracing during active MT. The stack walk treated `0x400000000000`
  as a frame function object and reached `gc2_registered_obj_valid()`.
- The failing path was:
  `gc2_registered_obj_valid -> lj_gc2_obj_valid -> gc_frame_func_valid ->
  gc_frame_prev_safe -> gc_mark_frame_chain_funcs -> gc_traverse_thread`.
- On FR2 stacks, frame function headers are tagged TValues in the slot before
  the frame metadata. Concurrent frame-chain walkers now verify that slot is a
  tagged function before converting it to a `GCobj *` in both legacy GC and GC2
  helper copies.
- This keeps remote/JIT frame-chain scans conservative: malformed or transient
  frame headers stop the bounded walk instead of feeding arbitrary canonical
  values to the GC object validator.

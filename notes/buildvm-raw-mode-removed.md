# BuildVM raw output mode removal

`buildvm -m raw` was a debug-only output mode that wrote the DynASM-produced
machine-code buffer directly to the selected file. The normal Linux, macOS, and
Windows build paths use assembly or object output (`elfasm`, `machasm`, and
`peobj`) and do not depend on this mode.

Removing the mode keeps instruction encoding work inside DynASM and the named
x86 emitter helpers. That matches the MT fork rule that new VM/JIT instruction
changes should be represented as assembler/frontend support instead of detached
encoded blobs.

Verification:

- `make -C src -j$(nproc)` rebuilds `host/buildvm` and the generated VM.
- `host/buildvm -h` no longer advertises `raw`.

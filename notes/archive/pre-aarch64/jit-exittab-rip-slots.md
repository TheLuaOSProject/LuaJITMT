# JIT x64 exittab RIP slots

Linux/x64 per-trace exit stubs now keep their exit target slots in the mcode
area and jump through them with `jmp qword ptr [rip+slot]`. Slot publication is
still a release store to mutable data, so side-trace attachment and flush keep
the lockless exittab model without patching parent mcode.

The mcode-owned slots are used only on Linux/x64, where secure builds have the
dual-map RW alias and insecure builds are writable. macOS and Windows keep the
heap-backed preserving stub because later exittab publication must not write
to executable mcode pages without the platform-specific protection protocol.

The trace body marks mcode-owned exittabs through mcode lifetime only: GC trace
marking and trace free skip heap mark/free for these slots.

Linux/x64 mcode dual-map mode cleanup

The supported secure Linux/x64 mcode path maps each area twice: RX for
published execution and RW for code generation/patch writes. Reopening an
existing area for generation or patching must not change the RX mapping's page
permissions; peer TGs may already be executing code from the same area.

This slice splits transaction mode bookkeeping from OS page-protection changes
in `lj_mcode.c`. Non-dual-map builds still use `mcode_setprot()` for
`MCPROT_GEN` / `MCPROT_RUN`, while dual-map builds update only `J->mcprot` and
continue writing through the RW alias.

`tests/t-jit-mcode-prot.c` now opens and closes a patch transaction on a live
mcode area and checks `/proc/self/maps` while the transaction is open. On the
Linux/x64 secure target, the RX mapping must remain executable and not writable,
and the RW mapping must remain writable and not executable.

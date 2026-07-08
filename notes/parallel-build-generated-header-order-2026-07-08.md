# Parallel Generated Header Order - 2026-07-08

Clean `make -C src -j2` could start target object compilation before all
`buildvm` generated headers had completed, surfacing as missing
`lj_libdef.h`/`lj_folddef.h` includes during M7 clean-build gates. Target core
objects now carry an explicit dependency on `ALL_HDRGEN`, so parallel builds
finish the generated header set before compiling the runtime objects that may
include them.

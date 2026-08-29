# Clang 19 ASan CX16 workaround (2026-07-17)

## Finding

Clang 19.1.7 miscompiled the inlined `__atomic_compare_exchange_n()` used by
`la_cas128()` when AddressSanitizer was enabled. In the failing code it saved
the real object address, loaded `desired.lo` into the same register, and then
used that desired value as the base of `lock cmpxchg16b`. A desired low word of
four therefore attempted to access address `0x24`.

The failure was reproducible in the standalone markword/token test with:

```sh
clang -std=gnu11 -O1 -g -Wall -Wextra -Werror -pthread -mcx16 \
  -fsanitize=address -fno-omit-frame-pointer -Isrc \
  tests/t-gc2-markword-token.c -o /tmp/t-gc2-markword-token-clang-asan
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:detect_stack_use_after_return=0 \
  /tmp/t-gc2-markword-token-clang-asan
```

The generated instruction used `%rbx == 4` as the base of
`lock cmpxchg16b 0x20(%rbx)`. `-mno-outline-atomics` is not applicable on
x86-64, and suppressing ASan instrumentation on the always-inlined wrapper did
not prevent the bad register allocation. A noinline wrapper did, as did the
existing explicit x86-64 inline-assembly implementation.

## Resolution

All supported x86-64 GCC and Clang builds now use the explicit
`lock cmpxchg16b` path in `la_cas128()`. This was already the required GCC path
to avoid an out-of-line `libatomic` call, so the change unifies the target
contract without changing memory ordering or the non-x86 fallback.

The strict optimized Clang, Clang UBSan, GCC sanitizer, and Clang ASan variants
must continue to be checked for an inline `cmpxchg16b` and absence of
`libatomic`/undefined `__atomic` imports.

# Ordinary HFA parity evidence (2026-10-29)

This is a follow-up to the bounded Darwin ARM64 two-float HFA named-argument slice. It uses ordinary assertion builds with no test-helper symbols or fixture-only runtime hooks. Source under test: `8c32bce2d1c7bfbafadd6dc0d2cc8b391d3086ac` (`codex/arm64-hfa-args`). Probe scripts came from root probe commit `1487ee15bd3c04c2e1a3b3fcba8bfecfea0a04ef`; the interpreter-oracle loaders recursively disable the loader prototype tree before creating returned/captured closures.

Evidence directory: `/Users/frityet/Scratch/ljmt-hfa-ordinary-20261029/`.

| Target | Flags | Runtime SHA-256 | Archive SHA-256 | Profile | Source suite | Imported stock bytecode |
|---|---|---|---|---|---:|---:|
| ARM64 | `-arch arm64`; `LUAJIT_MT_ARM64_BOOTSTRAP`, `LUAJIT_MT_ARM64_JIT_EXPERIMENTAL`, `LUA_USE_ASSERT` | `212e4ae0db4514bc13cc666b077813ea044de3e9d978cf6c43f85522ffed85db` | `f78c746acd0a170ca1bdf16d5087d1ac5fa9a4de382d124bb43c974330fc0937` | `02014e9605d1771217d61d81ffdf2a58297ead5885e7d529d8a8308b967e9ea7` | 509 passed | 509 passed |
| x86_64 | `-arch x86_64 -mcx16`; `LUA_USE_ASSERT` | `43fad16187c4d9a8376f7b84ea93b4252c649141db63fef0f47d7ad506418bd2` | `98c68ee637f7e38755513ccba388bb94f350e3cdf8d5a4ef469caf9244f9acc3` | `ea0b9a03075027035c2fc7e11ec203635460a9bbb51744cccd346c8581c88d12` | 509 passed | 509 passed |

Both targets ran `jit-aggregate-probe.lua`, `jit-topology-probe.lua`, and `jit-parity.lua` successfully. The two-float HFA argument case reports `CALLXS` in root trace 1 and a selected-root exit at `pc14:CGET`; the corresponding x86_64 case reports the same native-call/exit shape. This establishes ordinary-runtime HFA native entry and exit on ARM64, alongside the existing x86_64 witness. Aggregate probe: 25 oracle checks per case; HFA result remains outside the admitted ARM64 scope. The wider parity probe reports 20 sampled rows, with root-exit witnesses on 17 ARM64 rows and 18 x86_64 rows; missing row witnesses mean no selected-root exit was observed, not proof of absent native execution.

The topology probes completed all phases and oracle comparisons on both architectures. The full logs preserve trace IDs/topology and selected-root/other exits. `small.dylib` SHA-256 was `84d2abd3b9a63b3c5442f06db660424f9a8465825543ce93a3d96c9f13c3be8c` (universal `x86_64 arm64`). The original-v2 stock compiler was `/opt/homebrew/bin/luajit`, SHA-256 `8f0166d1a31d0ba2318357120d290ff2868612f8e03a8291e736711f6384981c`.

Logs and immutable binaries/modules are stored in the evidence directory. The shared profile helper restored the ordinary ARM64 assertion bootstrap/experimental profile after the x86_64 run. No tests from this matrix used a JIT TRACE observer; the benchmark scripts use JIT-off interpreter oracles and retain a live MT peer.

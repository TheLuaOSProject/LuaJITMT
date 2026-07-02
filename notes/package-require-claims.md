# Package peer claims

LuaJIT's public `package.loaded` contract is unchanged: `nil` and `false` mean
"not loaded", a truthy non-sentinel value is returned, the sentinel is written
only after a loader is found, recursive module execution sees the stock
"loop or previous error" error, and a module that returns `nil` only publishes
`true` if the sentinel is still present.

The lockless threading fork adds a private address-keyed registry table to
serialize peer OS threads that require the same module. The claim is
intentionally separate from `package.loaded`, because module code and custom
loaders can observe or mutate `package.loaded` and stock LuaJIT uses that table
as the public protocol. The claim table is not installed under a public string
key, so `debug.getregistry()` does not expose fork-local names or collide with
embedder string keys. Peer threads wait while a different thread owns the
private claim, then restart from the normal `package.loaded` check. Same-thread
recursion is not blocked by the private claim, so custom loaders can still
recurse before the public sentinel is published and modules can still exercise
the stock `package.loaded[name] = false/nil` reload behavior.

The owner clears the private claim on every protected exit from loader search or
module execution. If a module body errors after the public sentinel is written,
the sentinel remains in `package.loaded` as stock LuaJIT does; waiters wake and
report the normal "loop or previous error loading module" error instead of
blocking forever.

`package.loadlib()` uses the same private-claim pattern with a separate
address-keyed registry table keyed by library path. This serializes
`ll_register()` and the first `dlopen()` for peer OS threads so the registry's
`LOADLIB: path` userdata remains the single cached handle owner. The public
`package.loadlib()` API and `_LOADLIB` userdata behavior are unchanged; only
peer threads loading the same path wait and retry through the normal cache path.

Behavior coverage lives in `tests/t-threading-require-once.lua` and the
`m4_threading_require_once` suite case. It checks concurrent success, concurrent
error cleanup, false reloads, module recursion, and loader recursion before the
sentinel. `tests/t-loadlib-cache-race.c` and `m4_loadlib_cache_race` wrap
`dlopen()` and assert concurrent `package.loadlib()` calls open the target
shared object once. These are intentionally behavior fixtures rather than
source-text checks.

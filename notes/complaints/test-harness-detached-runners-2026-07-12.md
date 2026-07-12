# Aborted suite sessions can leave detached LuaJIT runners (2026-07-12)

Status: unresolved harness issue; stale processes were terminated manually.

Several invocations of `tools/test.lua m2_arena_all` and
`tools/test.lua m2_arena_state` printed `Aborted (core dumped)` to their tool
sessions, but the top-level `src/luajit` processes survived after the sessions
returned.  Four observed runners remained runnable at roughly 96--99 percent
CPU for between 10 and 20 minutes.  `TERM` stopped them cleanly.

The focused C fixtures do not reproduce this behavior when compiled and run
directly.  This appears to be process supervision in the suite/tool execution
path: an abort is reported without reliably reaping or terminating the whole
descendant process group.

The harness should run each suite invocation in a supervised process group and,
on timeout, abort, or tool-session loss, terminate and reap every descendant.
Until that is fixed, validation must inspect the process table after an aborted
suite and manually terminate only the stale runners belonging to that suite.

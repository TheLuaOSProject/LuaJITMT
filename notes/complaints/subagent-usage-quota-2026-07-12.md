# Shared subagent usage quota stops active reviews (2026-07-12)

Status: resolved the same day after the user reset the shared limits.  The
three interrupted agents resumed their existing scopes successfully.  The
details below remain as a harness incident record.

Three active repository agents (`body_lease_consumer_audit`,
`x64_lifetime_finalize`, and `x64_membership_independent_review`) terminated at
the same time with the environment error:

```
You've hit your usage limit. Visit https://chatgpt.com/codex/settings/usage to
purchase more credits or try again at Jul 18th, 2026 6:07 AM.
```

This is not a repository deadlock or a stuck tool process.  It prevents further
parallel delegation until the shared quota is restored, including follow-up
turns needed to finish already-assigned x64 and GC2 reviews.  The primary agent
can continue locally, but independent adversarial review and platform work now
run serially and lose the intended four-way throughput.

Please restore or separate the subagent allowance from the primary session so
long-running goal work can continue to use the advertised concurrency slots.

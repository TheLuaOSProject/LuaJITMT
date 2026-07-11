# GitHub push credentials unavailable (2026-07-11)

## Status

Resolved on 2026-07-11.

## Impact

The refreshed container now has an authenticated GitHub CLI credential helper.
The previously pending checkpoints through `5bb32ca7` were pushed to `v2.1`.

## Evidence

The earlier failure was:

```
fatal: could not read Username for 'https://github.com': No such device or address
```

The final acceptance checks now pass: `gh auth status` reports the active
repository-capable account, `git push --dry-run origin v2.1` succeeds, and the
local and remote `v2.1` tips both resolve to `5bb32ca7`.

## Requested environment fix

The environment now provides the requested persistent, noninteractive
repository write path.  The accepted options were:

- install and authenticate GitHub CLI, then run `gh auth setup-git`; or
- mount/configure an HTTPS credential helper; or
- mount a GitHub SSH key and known-host entry, then change `origin` to SSH.

No token is stored in the repository or image.

# GitHub push credentials unavailable (2026-07-11)

## Status

Open in the refreshed development container.

## Impact

Local commits work, but the required continual push checkpoint cannot reach
`origin`.  Commit `dad20fe1` (`Add epoch-tagged GC2 activation primitives`) is
currently committed on `v2.1` and pending push.

## Evidence

`git push origin v2.1` fails with:

```
fatal: could not read Username for 'https://github.com': No such device or address
```

The remote is HTTPS, no Git credential helper is configured, `gh` is absent,
there is no authenticated `~/.config/gh`, and no usable SSH identity is mounted.

## Requested environment fix

Provide one persistent, noninteractive repository write path in the container:

- install and authenticate GitHub CLI, then run `gh auth setup-git`; or
- mount/configure an HTTPS credential helper; or
- mount a GitHub SSH key and known-host entry, then change `origin` to SSH.

Do not put a token in the repository or image.  A successful
`git push --dry-run origin v2.1` is the acceptance check.

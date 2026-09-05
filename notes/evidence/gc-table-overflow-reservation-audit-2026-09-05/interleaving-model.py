#!/usr/bin/env python3
"""SC ordering illustration only. Not a runtime or reclamation proof."""
from dataclasses import dataclass, replace
import json


@dataclass(frozen=True)
class State:
    mode: str = "inline"
    inline_serial: int = 7
    inline_covered: bool = False
    inline_seen: frozenset = frozenset()
    wide_serial: int = 19
    wide_covered: bool = True  # Coverage left by the prior cell incarnation.
    wide_seen: frozenset = frozenset()
    payload: frozenset = frozenset({"Q"})
    queued: frozenset = frozenset({"Q"})
    scan_capture: tuple | None = None
    scan_seen: frozenset = frozenset()


def step(s, event):
    actor, action = event.split(".")
    if action == "write":
        return replace(s, payload=s.payload | {actor}), None
    if action == "bump":
        return replace(s, wide_serial=s.wide_serial + 1,
                       wide_covered=False, wide_seen=frozenset()), None
    if action == "promote":
        return replace(s, mode="wide", inline_covered=False), None
    if action == "queue":
        return replace(s, queued=s.queued | {actor}), None
    if action == "capture":
        serial = s.inline_serial if s.mode == "inline" else s.wide_serial
        return replace(s, scan_capture=(s.mode, serial)), None
    if action == "read":
        return replace(s, scan_seen=s.payload), None
    if action == "proof":
        if s.scan_capture == ("inline", s.inline_serial) and s.mode == "inline":
            return replace(s, inline_covered=True, inline_seen=s.scan_seen), None
        if s.scan_capture == ("wide", s.wide_serial) and s.mode == "wide":
            return replace(s, wide_covered=True, wide_seen=s.scan_seen), None
        return s, None
    if action == "consume":
        covered = s.inline_covered if s.mode == "inline" else s.wide_covered
        seen = s.inline_seen if s.mode == "inline" else s.wide_seen
        if covered and not s.queued.issubset(seen):
            return s, {"queued": sorted(s.queued), "proof_covers": sorted(seen),
                       "mode": s.mode}
        return s, None
    raise AssertionError(event)


def explore(order):
    chains = [
        tuple("A." + event for event in order),
        tuple("B." + event for event in order),
        ("S.capture", "S.read", "S.proof"),
        ("C.consume",),
    ]
    visited = set()
    complete = 0
    first_failure = None

    def walk(s, positions, trace):
        nonlocal complete, first_failure
        key = (s, positions)
        if key in visited:
            return
        visited.add(key)
        if all(positions[i] == len(chains[i]) for i in range(len(chains))):
            complete += 1
            return
        for i, chain in enumerate(chains):
            if positions[i] >= len(chain):
                continue
            event = chain[positions[i]]
            after, failure = step(s, event)
            pos = list(positions)
            pos[i] += 1
            next_trace = trace + [event]
            if failure is not None and first_failure is None:
                first_failure = {"trace": next_trace, "failure": failure}
            walk(after, tuple(pos), next_trace)

    walk(State(), (0, 0, 0, 0), [])
    return {"writer_order": order, "visited_states": len(visited),
            "terminal_states": complete, "first_failure": first_failure}


bad = explore(("write", "promote", "bump", "queue"))
good = explore(("write", "bump", "promote", "queue"))
assert bad["first_failure"] is not None
assert good["first_failure"] is None
old_wide = (2, 1)
renewed_wide = (3, 1)
assert old_wide[1] == renewed_wide[1] and old_wide != renewed_wide
print(json.dumps({
    "scope": "SC logical ordering model; no GC/lifetime/weak-memory validation",
    "post_promotion_bump_with_missed_reuse_reset": bad,
    "persistent_monotone_wide_pre_promotion_bump": good,
    "era_comparison_control": {
        "old": old_wide, "renewed": renewed_wide,
        "low_serial_only_accepts": True, "full_identity_accepts": False,
    },
}, indent=2))


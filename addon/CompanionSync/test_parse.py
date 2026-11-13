"""The Comp sync grammar, tested without a client.

This mirrors the parser in Core.lua line for line: split the payload on '|', then each
entry on ',' into name,class,level,quests,flags,free. It cannot catch a Lua typo - run
`/wlcg test` in game for that - but it does catch a change in the payload that would break
the window, which is the failure that matters after a server-side edit.

    python test_parse.py
"""
from __future__ import annotations

import re
import sys

FIELD = re.compile(r"^([^,]+),([^,]*),([^,]*),([^,]*),([^,]*),([^,]*),([^,]*)$")


def parse_payload(payload: str) -> list[dict]:
    """Same rules as G.ParseCompPayload, including the field count."""
    rows = []
    for entry in [e for e in payload.split("|") if e]:
        match = FIELD.match(entry)
        if not match:
            continue

        name, class_id, race_id, level, quests, flags, free = match.groups()
        if not name:
            continue

        rows.append(
            {
                "name": name,
                "classId": int(class_id or 0),
                "raceId": int(race_id or 0),
                "level": int(level or 0),
                "quests": int(quests or 0),
                "flags": flags or "-",
                "free": int(free or 0),
            }
        )

    # online first, then by name - the same order the window shows
    rows.sort(key=lambda r: (0 if "o" in r["flags"] else 1, r["name"]))
    return rows


def join_chunks(parts: list[str]) -> str:
    """Comp sync i/n arrives in order; joining is a plain concatenation with '|'."""
    return "|".join(parts)


CASES = [
    # a real line, straight out of the server log
    (
        "Healy,5,1,1,0,ogs,110|Soak,9,2,1,0,ogs,105",
        [
            {"name": "Healy", "classId": 5, "raceId": 1, "level": 1, "quests": 0, "flags": "ogs", "free": 110},
            {"name": "Soak", "classId": 9, "raceId": 2, "level": 1, "quests": 0, "flags": "ogs", "free": 105},
        ],
    ),
    # offline companion: only a name, everything else zeroed, no 'o' flag
    (
        "Healy,5,1,1,1,ogs,110|Ghost,0,0,0,0,-,0",
        [
            {"name": "Healy", "classId": 5, "raceId": 1, "level": 1, "quests": 1, "flags": "ogs", "free": 110},
            {"name": "Ghost", "classId": 0, "raceId": 0, "level": 0, "quests": 0, "flags": "-", "free": 0},
        ],
    ),
    # behind on your quests, not grouped
    (
        "Healy,5,1,32,4,b,88",
        [{"name": "Healy", "classId": 5, "raceId": 1, "level": 32, "quests": 4, "flags": "b", "free": 88}],
    ),
]


def main() -> int:
    failures = 0

    for payload, expected in CASES:
        got = parse_payload(payload)
        if got != expected:
            failures += 1
            print(f"FAIL parse {payload!r}\n  expected {expected}\n  got      {got}")

    # a name with a space survives, because the split is on ',' not whitespace
    if parse_payload("Big Bob,1,1,5,0,o,10")[0]["name"] != "Big Bob":
        failures += 1
        print("FAIL a name with a space did not survive")

    # an entry missing a field is dropped rather than half-parsed
    if parse_payload("Broken,1,2,3,o"):
        failures += 1
        print("FAIL a short entry was accepted")

    # chunked payloads
    joined = join_chunks(["Healy,5,1,1,0,ogs,110", "Soak,9,2,1,0,ogs,105"])
    if len(parse_payload(joined)) != 2:
        failures += 1
        print("FAIL chunked join lost an entry")

    # ordering: online first, then alphabetical
    ordered = [r["name"] for r in parse_payload("Zed,1,1,1,0,-,0|Amy,1,1,1,0,o,0")]
    if ordered != ["Amy", "Zed"]:
        failures += 1
        print(f"FAIL ordering was {ordered}")

    total = len(CASES) + 4
    print(f"{total - failures}/{total} payload cases passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

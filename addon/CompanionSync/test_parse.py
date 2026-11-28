"""The Comp sync grammar, tested without a client.

This mirrors the parser in Core.lua line for line: split the payload on '|', then each
entry on ',' into name,class,race,level,quests,flags,free. It cannot catch a Lua typo - run
`/csync test` in game for that - but it does catch a change in the payload that would break
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
    # no-gather mode: the 'p' flag rides the flags run - the card shows the auto badge
    (
        "Healy,5,1,12,2,ogsp,64|Ghost,0,0,0,0,-,0",
        [
            {"name": "Healy", "classId": 5, "raceId": 1, "level": 12, "quests": 2, "flags": "ogsp", "free": 64},
            {"name": "Ghost", "classId": 0, "raceId": 0, "level": 0, "quests": 0, "flags": "-", "free": 0},
        ],
    ),
]


SPEC = re.compile(r"^([^>]+)>?([^@]*)@?(\d*)(.*)$")


def parse_specs(payload: str) -> dict:
    """Same rules as the Specs branch in Core.lua: name=have[>want@min][!low|!off]."""
    out = {}
    for pair in [p for p in payload.split("|") if p]:
        name, _, rest = pair.partition("=")
        if not name or not rest:
            continue

        match = SPEC.match(rest)
        if not match:
            continue

        have, want, minimum, flag = match.groups()
        entry = {"have": have}
        if want:
            entry["want"] = want
        if minimum:
            entry["min"] = int(minimum)
        if "!low" in (flag or ""):
            entry["low"] = True
        if "!off" in (flag or ""):
            entry["off"] = True
        out[name] = entry
    return out


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

    # the death flag: a dead companion parses and keeps its flag visible
    dead = parse_payload("Wounded,5,1,9,2,ogd,16")
    if len(dead) != 1 or "d" not in dead[0].get("flags", ""):
        failures += 1
        print(f"FAIL dead flag lost: {dead}")

    # ordering: online first, then alphabetical
    ordered = [r["name"] for r in parse_payload("Zed,1,1,1,0,-,0|Amy,1,1,1,0,o,0")]
    if ordered != ["Amy", "Zed"]:
        failures += 1
        print(f"FAIL ordering was {ordered}")

    # Roles flags: "name=role/policy[:na][:q][:l<seconds>]" - ":l" is the server's role-change
    # window, and the addon counts it down instead of looking broken
    roles = "Vorrin=dps:l42|Branros=support/joined:q:na|Thalthas=dps/never"
    parsed = {}
    for pair in [p for p in roles.split("|") if p]:
        name, rest = pair.split("=", 1)
        role, _, flags = re.match(r"^(\w+)/?(\w*)(.*)$", rest).groups()
        parsed[name] = (role, flags)
    if parsed.get("Vorrin") != ("dps", ":l42") or parsed.get("Branros") != ("support", ":q:na"):
        failures += 1
        print(f"FAIL roles flags: {parsed}")
    if not re.search(r":l(\d+)", parsed.get("Vorrin", ("", ""))[1]):
        failures += 1
        print("FAIL role window not readable")

    # the Specs line: what the talents are against what the role asks, and why a role has not
    # reached them yet - "!low" is the server's level gate, "!off" a log-only server
    spec_cases = [
        ("Vorrin=fury", {"Vorrin": {"have": "fury"}}),
        ("Vorrin=fury>protection@40", {"Vorrin": {"have": "fury", "want": "protection", "min": 40}}),
        ("Vorrin=fury>protection@40!low",
         {"Vorrin": {"have": "fury", "want": "protection", "min": 40, "low": True}}),
        ("Vorrin=fury>protection@40!off",
         {"Vorrin": {"have": "fury", "want": "protection", "min": 40, "off": True}}),
        ("Bess=beast mastery>marksmanship@40|Vorrin=none>arms@40!low",
         {"Bess": {"have": "beast mastery", "want": "marksmanship", "min": 40},
          "Vorrin": {"have": "none", "want": "arms", "min": 40, "low": True}}),
    ]
    for payload, expected in spec_cases:
        got = parse_specs(payload)
        if got != expected:
            failures += 1
            print(f"FAIL specs {payload!r}\n  expected {expected}\n  got      {got}")

    total = len(CASES) + 6 + len(spec_cases)
    print(f"{total - failures}/{total} payload cases passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

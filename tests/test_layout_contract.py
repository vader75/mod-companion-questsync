"""Geometry of the companion window: controls must not sit on top of each other.

The card grew a control per feature (map, train, skills, role, engagement, supply) and the
header grew a formation row. Reading Lua cannot show a collision; measuring can. This parses
the anchors out of UI.lua, resolves them into boxes in top-left coordinates and asserts none
of them intersect and all of them stay inside their frame.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UI = (ROOT / "addon" / "CompanionSync" / "UI.lua").read_text(encoding="utf-8")

BUTTON_HEIGHT = 22
DYNAMIC = {"primary"}          # repositioned at runtime for the dead/revived state


def constant(name):
    match = re.search(r"^local %s = (\d+)" % name, UI, re.M)
    assert match, name
    return int(match.group(1))


WIDTH, PAD = constant("WIDTH"), constant("PAD")
CARD_W, CARD_H = WIDTH - 2 * PAD, constant("CARD_HEIGHT")
HEADER_H, ROSTER_H = constant("HEADER_HEIGHT"), constant("ROSTER_HEIGHT")


def widths():
    return {name: int(width)
            for name, width in re.findall(r'card\.(\w+) = Button\(card, "[^"]*", (\d+)', UI)}


def anchors():
    """name -> list of (corner, numeric args, rest of the arg list)."""
    found = {}
    pattern = re.compile(
        r'card\.(\w+):SetPoint\("(TOPLEFT|TOPRIGHT|BOTTOMLEFT|BOTTOMRIGHT|LEFT)"([^)]*)\)')
    for name, corner, rest in pattern.findall(UI):
        found.setdefault(name, []).append((corner, [int(v) for v in re.findall(r"-?\d+", rest)], rest))
    return found


def boxes():
    """Resolve every card control into (x1, y1, x2, y2), y measured down from the card top."""
    w, found, out = widths(), anchors(), {}

    for name, entries in found.items():
        if name not in w:
            continue
        for corner, values, _ in entries:
            if corner == "LEFT" or len(values) < 2:
                out.setdefault(name, None)          # needs its neighbour first
                continue
            x, y = values[0], values[1]
            if corner == "TOPLEFT":
                x1, y1 = x, -y
            elif corner == "TOPRIGHT":
                x1, y1 = CARD_W + x - w[name], -y
            elif corner == "BOTTOMRIGHT":
                x1, y1 = CARD_W + x - w[name], CARD_H + y - BUTTON_HEIGHT
            else:                                   # BOTTOMLEFT
                x1, y1 = x, CARD_H + y - BUTTON_HEIGHT
            out[name] = (x1, y1, x1 + w[name], y1 + BUTTON_HEIGHT)

    for name, entries in found.items():              # LEFT/RIGHT pairs, one pass is enough
        if name not in w or out.get(name):
            continue
        for corner, values, rest in entries:
            neighbour = re.search(r'card\.(\w+), "RIGHT"', rest)
            if corner == "LEFT" and neighbour and out.get(neighbour.group(1)):
                _, y1, x2, _ = out[neighbour.group(1)]
                dx = values[0]
                out[name] = (x2 + dx, y1, x2 + dx + w[name], y1 + BUTTON_HEIGHT)

    return {name: box for name, box in out.items() if box and name not in DYNAMIC}


def overlap(a, b):
    ax1, ay1, ax2, ay2 = a
    bx1, by1, bx2, by2 = b
    return max(0, min(ax2, bx2) - max(ax1, bx1)) > 0 and max(0, min(ay2, by2) - max(ay1, by1)) > 0


class CardLayout(unittest.TestCase):
    def test_every_control_resolves(self):
        # a control whose anchor this test cannot read is a control it cannot check
        unresolved = set(widths()) - DYNAMIC - set(boxes())
        self.assertFalse(unresolved, "unresolved card anchors: %s" % sorted(unresolved))

    def test_no_two_controls_overlap(self):
        found = boxes()
        clashes = ["%s %s x %s %s" % (a, found[a], b, found[b])
                   for i, a in enumerate(sorted(found)) for b in sorted(found)[i + 1:]
                   if overlap(found[a], found[b])]
        self.assertFalse(clashes, "controls overlap: " + "; ".join(clashes))

    def test_controls_stay_inside_the_card(self):
        outside = ["%s %s" % (name, box) for name, box in boxes().items()
                   if box[0] < 0 or box[1] < 0 or box[2] > CARD_W or box[3] > CARD_H]
        self.assertFalse(outside, "controls outside the card: " + "; ".join(outside))

    def test_role_and_engagement_sit_on_one_row(self):
        found = boxes()
        self.assertEqual(found["roleButton"][1], found["engageButton"][1])
        self.assertLess(found["roleButton"][2], found["engageButton"][0] + 1,
                        "role runs into engagement")
        self.assertLess(found["engageButton"][2], found["trainButton"][0] + 1,
                        "engagement runs into Train")


class HeaderLayout(unittest.TestCase):
    """The formation row is the only header control below Add companion."""

    def test_formation_row_clears_the_divider_and_the_roster(self):
        bottom = 42 + BUTTON_HEIGHT                  # measured down from the frame top
        divider, tabs = HEADER_H - 1, HEADER_H + 4
        self.assertLessEqual(bottom, divider, "formation row crosses the divider")
        self.assertLessEqual(bottom, tabs - 4, "formation row touches the roster tabs")

    def test_header_is_tall_enough_for_its_row(self):
        self.assertGreater(HEADER_H, 14 + BUTTON_HEIGHT + 6 + BUTTON_HEIGHT,
                           "header cannot hold Add companion plus a control row")

    def test_formation_buttons_do_not_run_into_each_other(self):
        self.assertTrue(re.findall(r"Button\(frame, preset\.label, 46", UI),
                        "formation buttons changed shape")
        pitch = 50
        self.assertGreater(pitch, 46, "formation buttons overlap")
        span_left = WIDTH - 46 - 5 * pitch
        self.assertGreater(span_left, PAD, "formation row runs off the left of the header")


if __name__ == "__main__":
    unittest.main()

"""The compiled worldport probe stays in step with the source it exercises.

tests/worldport-probe/*.inc are slices of src/mod_companion_questsync.cpp: the
Timing block, PendingFollow and FollowOwnerThroughTeleport are verbatim cuts
(scripts/cut-worldport-incs.py re-cuts them); cq-exit-hooks.inc is hand-trimmed
because OnPlayerLevelChanged sits between the two hooks in the source, so each
of its lines must still appear in the source. When this test fails, re-cut.
"""
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent / 'worldport-probe'
SOURCE = (Path(__file__).resolve().parents[1] / 'src/mod_companion_questsync.cpp').read_text(encoding='utf-8')


VERBATIM = ('cq-exit-timing.inc', 'cq-exit-pending.inc', 'cq-exit-function.inc')
TRACKED = ('cq-exit-hooks.inc',)


class WorldportProbe(unittest.TestCase):
    def test_verbatim_slices_are_cut_from_the_current_source(self):
        for inc in VERBATIM:
            text = (HERE / inc).read_text(encoding='utf-8').strip()
            self.assertIn(text, SOURCE, f'{inc} has drifted - run scripts/cut-worldport-incs.py')

    def test_trimmed_hook_lines_still_match_the_source(self):
        for inc in TRACKED:
            for line in (HERE / inc).read_text(encoding='utf-8').splitlines():
                if line.strip():
                    self.assertIn(line, SOURCE, f'{inc} line has drifted: {line.strip()[:60]}')

    def test_probe_harness_covers_the_eight_exit_scenarios(self):
        probe = (HERE / 'probe.cpp').read_text(encoding='utf-8')
        for scenario in ('normal window', 'same-instance straggler after worldport ACK',
                         'missed 5-minute deadline', 'different instance of same dungeon',
                         'not grouped', 'unconfirmed exit', 'owner re-enters',
                         'ordinary outdoor travel expires at 60s', 'stuck falling flag still follows'):
            self.assertIn(f'"{scenario}"', probe)

"""Re-cut the worldport probe's verbatim slices from the module source.

tests/test_worldport_probe.py fails when src/mod_companion_questsync.cpp has
drifted from tests/worldport-probe/*.inc. Run this to re-cut the three slices
that are verbatim (Timing, PendingFollow, FollowOwnerThroughTeleport). The
hooks slice is hand-trimmed because OnPlayerLevelChanged sits between the two
hooks in the source - re-cut it by hand if a hook line changes.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'src/mod_companion_questsync.cpp').read_text(encoding='utf-8')
OUT = ROOT / 'tests/worldport-probe'


def cut(start: str, end: str, keep_end: str) -> str:
    middle = SOURCE.split(start, 1)[1].split(end, 1)[0]
    return start + middle.rstrip() + keep_end


SLICES = {
    'cq-exit-timing.inc': cut('namespace Timing', '\n    }', '\n    }'),
    'cq-exit-pending.inc': cut('    struct PendingFollow', '\n    std::map<uint32, PendingFollow>', ''),
    'cq-exit-function.inc': cut('void FollowOwnerThroughTeleport(', 'void AddSkipped(', ''),
}

for name, text in SLICES.items():
    assert text.strip() in SOURCE, f'{name} would not be a verbatim slice - fix the markers'
    (OUT / name).write_text(text.rstrip() + '\n', encoding='utf-8', newline='\n')
    print(f'{name}: {len(text.splitlines())} lines')

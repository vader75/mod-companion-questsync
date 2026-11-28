"""Trim the ResourceCompile include list that overflows rc.exe (MSB6003).

CMake regeneration writes worldserver.vcxproj with a ResourceCompile
AdditionalIncludeDirectories list carrying every include directory of the
tree - thousands of entries - and MSBuild then fails with MSB6003 because
rc.exe's command line overflows. rc.exe only needs the build root (for the
generated headers) and the server apps directory (for the .rc sources), so this
script trims each ResourceCompile block to those two and re-runs are no-ops.

Run it after any CMake regenerate / install-to-server refresh, before building:

    python scripts/fix-resource-includes.py
    python scripts/fix-resource-includes.py --vcxproj D:/Build/build/src/server/apps/worldserver.vcxproj

CMake regeneration re-bloats the file; just run the script again.
"""
import argparse
import re
import sys
from pathlib import Path

DEFAULT_VCXPROJ = Path('D:/Build/build/src/server/apps/worldserver.vcxproj')
DEFAULT_SOURCE_ROOT = Path('D:/Build/WOW_Legends_Server_Source')
# A regenerated list is thousands of entries; a healthy one is a handful.
THRESHOLD = 12


def keep_entries(vcxproj: Path, source_root: Path) -> list:
    build_root = vcxproj.resolve().parents[3]          # .../src/server/apps/worldserver.vcxproj
    source_apps = source_root / 'src' / 'server' / 'apps'
    for path in (build_root, source_apps):
        if not path.is_dir():
            sys.exit(f'error: kept include directory does not exist: {path}')
    return [str(build_root), str(source_apps), '%(AdditionalIncludeDirectories)']


def fix(text: str, keep: str) -> tuple:
    changed = 0

    def shorten(match: re.Match) -> str:
        nonlocal changed
        block = match.group(0)
        listed = re.search(r'AdditionalIncludeDirectories>(.*?)</AdditionalIncludeDirectories>', block, re.S)
        if not listed or listed.group(1).count(';') + 1 <= THRESHOLD:
            return block
        changed += 1
        return block.replace(listed.group(1), keep)

    return re.sub(r'<ResourceCompile>.*?</ResourceCompile>', shorten, text, flags=re.S), changed


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--vcxproj', type=Path, default=DEFAULT_VCXPROJ)
    parser.add_argument('--source-root', type=Path, default=DEFAULT_SOURCE_ROOT)
    args = parser.parse_args()

    if not args.vcxproj.is_file():
        sys.exit(f'error: vcxproj not found: {args.vcxproj}')

    keep = ';'.join(keep_entries(args.vcxproj, args.source_root))
    text = args.vcxproj.read_text(encoding='utf-8')
    fixed, changed = fix(text, keep)
    if not changed:
        print(f'{args.vcxproj}: ResourceCompile include lists already short, nothing to do')
        return

    args.vcxproj.write_text(fixed, encoding='utf-8', newline='')
    print(f'{args.vcxproj}: trimmed {changed} ResourceCompile block(s) to {keep}')


if __name__ == '__main__':
    main()

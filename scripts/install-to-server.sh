#!/usr/bin/env bash
# Sync this module into a server source tree.
#
#     scripts/install-to-server.sh /path/to/WOW_Legends_Server_Source
#     scripts/install-to-server.sh /path/to/server-src /path/to/client/Interface/AddOns
#
# The module is copied to <server>/modules/mod-companion-questsync. That directory is
# where the worldserver build picks it up. If a second argument is given, the addon is
# copied to it as well.
#
# Why a script: the same module existed in two tracked repositories and drifted about a
# thousand lines apart before anyone noticed. One command makes the copy explicit.
set -euo pipefail

if [ $# -lt 1 ]; then
    echo "usage: $0 <server-source-tree> [client AddOns directory]" >&2
    exit 2
fi

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
server="$1"
target="$server/modules/mod-companion-questsync"

if [ ! -d "$server/modules" ]; then
    echo "not a server source tree (no modules/): $server" >&2
    exit 2
fi

mkdir -p "$target"
for part in src conf sql README.md CMakeLists.txt LICENSE; do
    if [ -e "$repo/$part" ]; then
        rm -rf "${target:?}/$part"
        cp -r "$repo/$part" "$target/$part"
        echo "  $part"
    fi
done

echo "module -> $target"

if [ $# -ge 2 ]; then
    addons="$2"
    mkdir -p "$addons/WLCompanionGroup"
    cp "$repo"/addon/WLCompanionGroup/* "$addons/WLCompanionGroup/"
    echo "addon  -> $addons/WLCompanionGroup"
fi

echo
echo "Two things the copy cannot do for you:"
echo "  * re-run CMake, which the module list needs (configure, then build worldserver)"
echo "  * import sql/companion-questsync.sql if this is a new install"

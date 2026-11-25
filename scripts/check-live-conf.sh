#!/usr/bin/env bash
# Fails when the server's module conf has fallen behind the shipped one.
#
# The core reads configs/modules/<module>.conf at boot; a key that only exists in the repo's
# conf/*.conf.dist is silently ignored and the code silently uses its default - which is how a
# deployed server ends up running yesterday's settings while looking correct from the outside.
#
# Usage: scripts/check-live-conf.sh [server-dir]
#        defaults to $WL_SERVER_DIR, then D:/Applications/wow-legends-server
set -u

repo="$(cd "$(dirname "$0")/.." && pwd)"
server="${1:-${WL_SERVER_DIR:-D:/Applications/wow-legends-server}}"
dist="$repo/conf/mod_companion_questsync.conf.dist"
live="$server/configs/modules/mod_companion_questsync.conf"

[ -f "$dist" ] || { echo "check-live-conf: no $dist" >&2; exit 2; }
[ -f "$live" ] || { echo "check-live-conf: no $live (module not deployed?)" >&2; exit 2; }

# only real assignments: prose that merely mentions a retired key is not a setting
keys() { grep -oE '^#?[[:space:]]*CompanionQuestSync\.[A-Za-z]+[[:space:]]*=' "$1" | sed 's/^#//; s/[[:space:]]*=.*$//; s/^[[:space:]]*//' | sort -u; }

missing="$(comm -13 <(keys "$live") <(keys "$dist"))"
stale_expectations="$(comm -23 <(keys "$live") <(keys "$dist"))"

if [ -n "$missing" ]; then
    echo "STALE CONFIG - $live is missing $(echo "$missing" | wc -l) option(s):"
    echo "$missing" | sed 's/^/  /'
    echo "$live and $server/configs/modules/mod_companion_questsync.conf.dist need refreshing before"
    echo "this deploy is called done."
    exit 1
fi

if [ -n "$stale_expectations" ]; then
    echo "note: $live still documents options this build no longer reads:"
    echo "$stale_expectations" | sed 's/^/  /'
fi

echo "config current: $(keys "$live" | wc -l) options, none missing from $live"

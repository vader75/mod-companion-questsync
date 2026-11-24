# Keep a companion's quest log and level in step with its owner.
#
# AzerothCore module registration. Sources are collected from src/ by the module
# system; AC_ADD_SCRIPT registers the script loader, AC_ADD_CONFIG_FILE deploys the
# .conf.dist into the server's module config directory.
#
# The playerbots integration (sell vendor trash, wear upgrades, item-usage verdict)
# is compiled only when MOD_PLAYERBOTS is defined, which the modules build sets when
# mod-playerbots is present. The playerbots HEADERS are still included unconditionally
# (ItemUsageValue.h, PlayerbotAI.h, Playerbots.h), so a build without that module's
# source on the include path will not compile: keep mod-playerbots checked out beside
# this module even if you leave the AI-backed behaviour compiled out.

AC_ADD_SCRIPT("${CMAKE_CURRENT_LIST_DIR}/src")

AC_ADD_CONFIG_FILE("${CMAKE_CURRENT_LIST_DIR}/conf/mod_companion_questsync.conf.dist")

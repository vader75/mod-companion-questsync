# Keep a companion's quest log and level in step with its owner.
#
# AzerothCore module registration. Sources are collected from src/ by the module
# system; AC_ADD_SCRIPT registers the script loader, AC_ADD_CONFIG_FILE deploys the
# .conf.dist into the server's module config directory.
#
# The playerbots integration (sell vendor trash, wear upgrades, item-usage verdict)
# is compiled only when mod-playerbots is present: the modules build defines
# MOD_PLAYERBOTS for every module in that case. Without it the module still builds
# and still syncs quests — it just cannot ask the bot's own AI to tidy its bags.

AC_ADD_SCRIPT("${CMAKE_CURRENT_LIST_DIR}/src")

AC_ADD_CONFIG_FILE("${CMAKE_CURRENT_LIST_DIR}/conf/mod_companion_questsync.conf.dist")

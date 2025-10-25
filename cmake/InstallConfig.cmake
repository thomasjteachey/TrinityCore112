# This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
#
# This file is free software; as a special exception the author gives
# unlimited permission to copy and/or distribute it, with or without
# modifications, as long as this notice is preserved.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY, to the extent permitted by law; without even the
# implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

# Additional module configuration (.conf.dist) files that should be installed
# alongside the core configuration templates.
if(NOT DEFINED TRINITYCORE_MODULE_CONF_DIST)
  set(TRINITYCORE_MODULE_CONF_DIST "")
endif()

list(APPEND TRINITYCORE_MODULE_CONF_DIST
  ${CMAKE_SOURCE_DIR}/conf/AutoBalance.conf.dist)
list(REMOVE_DUPLICATES TRINITYCORE_MODULE_CONF_DIST)

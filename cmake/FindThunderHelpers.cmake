# - Try to find ThunderHelpers
# Once done this will define
# ThunderHelpers_FOUND - System has ThunderHelpers
# ThunderHelpers_INCLUDE_DIRS - The ThunderHelpers include directories
# ThunderHelpers_LIBRARIES - The libraries needed to use ThunderHelpers
#
# Also creates an imported target:
# Thunder::ThunderHelpers

find_library(ThunderHelpers_LIBRARIES
    NAMES ThunderHelpers
    PATH_SUFFIXES thunder/plugins)

find_path(ThunderHelpers_INCLUDE_DIRS
    NAMES UtilsLogging.h
    PATH_SUFFIXES thunder/helpers)

set(ThunderHelpers_LIBRARIES
    ${ThunderHelpers_LIBRARIES}
    CACHE PATH "Path to ThunderHelpers library")

set(ThunderHelpers_INCLUDE_DIRS ${ThunderHelpers_INCLUDE_DIRS} CACHE PATH "Path to ThunderHelpers includes")

include(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(ThunderHelpers DEFAULT_MSG
    ThunderHelpers_INCLUDE_DIRS
    ThunderHelpers_LIBRARIES)

if(ThunderHelpers_FOUND AND NOT TARGET Thunder::ThunderHelpers)
    add_library(Thunder::ThunderHelpers SHARED IMPORTED)
    set_target_properties(Thunder::ThunderHelpers PROPERTIES
        IMPORTED_LOCATION "${ThunderHelpers_LIBRARIES}"
        INTERFACE_INCLUDE_DIRECTORIES "${ThunderHelpers_INCLUDE_DIRS}")
endif()

mark_as_advanced(
    ThunderHelpers_FOUND
    ThunderHelpers_INCLUDE_DIRS
    ThunderHelpers_LIBRARIES)

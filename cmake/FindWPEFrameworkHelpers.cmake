# - Try to find WPEFrameworkHelpers
# Once done this will define
# WPEFrameworkHelpers_FOUND - System has WPEFrameworkHelpers
# WPEFrameworkHelpers_INCLUDE_DIRS - The WPEFrameworkHelpers include directories
# WPEFrameworkHelpers_LIBRARIES - The libraries needed to use WPEFrameworkHelpers
#
# Also creates an imported target:
# WPEFramework::WPEFrameworkHelpers

find_library(WPEFrameworkHelpers_LIBRARIES
    NAMES WPEFrameworkHelpers
    PATH_SUFFIXES wpeframework/plugins)

find_path(WPEFrameworkHelpers_INCLUDE_DIRS
    NAMES UtilsLogging.h
    PATH_SUFFIXES wpeframework/helpers)

set(WPEFrameworkHelpers_LIBRARIES
    ${WPEFrameworkHelpers_LIBRARIES}
    CACHE PATH "Path to WPEFrameworkHelpers library")

set(WPEFrameworkHelpers_INCLUDE_DIRS ${WPEFrameworkHelpers_INCLUDE_DIRS} CACHE PATH "Path to WPEFrameworkHelpers includes")

include(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(WPEFrameworkHelpers DEFAULT_MSG
    WPEFrameworkHelpers_INCLUDE_DIRS
    WPEFrameworkHelpers_LIBRARIES)

if(WPEFrameworkHelpers_FOUND AND NOT TARGET WPEFramework::WPEFrameworkHelpers)
    add_library(WPEFramework::WPEFrameworkHelpers SHARED IMPORTED)
    set_target_properties(WPEFramework::WPEFrameworkHelpers PROPERTIES
        IMPORTED_LOCATION "${WPEFrameworkHelpers_LIBRARIES}"
        INTERFACE_INCLUDE_DIRECTORIES "${WPEFrameworkHelpers_INCLUDE_DIRS}")
endif()

mark_as_advanced(
    WPEFrameworkHelpers_FOUND
    WPEFrameworkHelpers_INCLUDE_DIRS
    WPEFrameworkHelpers_LIBRARIES)

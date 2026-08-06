# - Try to find DS (DeviceSettings)
# Once done this will define
# DS_FOUND - System has DS
# DS_INCLUDE_DIRS - The DS include directories
# DS_LIBRARIES - The libraries needed to use DS

find_package(PkgConfig)

find_library(DS_LIBRARIES NAMES ds)
find_library(DSHAL_LIBRARIES NAMES dshalcli)
find_library(OEMHAL_LIBRARIES NAMES ds-hal)
find_library(IARMBUS_LIBRARIES NAMES IARMBus)
find_path(DS_INCLUDE_DIRS NAMES manager.hpp PATH_SUFFIXES rdk/ds)
find_path(DSHAL_INCLUDE_DIRS NAMES dsTypes.h PATH_SUFFIXES rdk/halif/ds-hal)
find_path(DSRPC_INCLUDE_DIRS NAMES dsMgr.h PATH_SUFFIXES rdk/ds-rpc)

set(DS_LIBRARIES ${DS_LIBRARIES} ${DSHAL_LIBRARIES})
set(DS_LIBRARIES ${DS_LIBRARIES} CACHE PATH "Path to DS library")
set(DS_INCLUDE_DIRS ${DS_INCLUDE_DIRS} ${DSHAL_INCLUDE_DIRS} ${DSRPC_INCLUDE_DIRS})
set(DS_INCLUDE_DIRS ${DS_INCLUDE_DIRS} CACHE PATH "Path to DS include")

include(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(DS DEFAULT_MSG DS_INCLUDE_DIRS DS_LIBRARIES)

mark_as_advanced(
    DS_FOUND
    DS_INCLUDE_DIRS
    DS_LIBRARIES
    DS_LIBRARY_DIRS
    DS_FLAGS)

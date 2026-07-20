# If not stated otherwise in this file or this component's LICENSE file the
# following copyright and licenses apply:
#
# Copyright 2025 RDK Management
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set(PLUGIN_NAME AudioOutput)
set(MODULE_NAME ${NAMESPACE}${PLUGIN_NAME})
set(PLUGIN_IMPLEMENTATION ${MODULE_NAME}Implementation)

set(PLUGIN_AUDIOOUTPUT_AUTOSTART "false" CACHE STRING "Automatically start AudioOutput plugin")
set(PLUGIN_AUDIOOUTPUT_MODE "Off" CACHE STRING "AudioOutput execution mode (Off=in-process, LOCAL=out-of-process)")
set(PLUGIN_AUDIOOUTPUT_STARTUPORDER "" CACHE STRING "Startup order for AudioOutput plugin")

# ----------------------------------------------------------------
# DS_COMRPC — build selection flag
#
# OFF (default): use DS_IARM/AudioOutputImplementation.cpp
#               Links against libds / dshal (legacy path).
# ON:           use DS_COMRPC/AudioOutputImplementation.cpp
#               Uses COM-RPC to entservices-devicesettings; no libds link.
#               Enable via OEM_MAKE Yocto recipe:
#                 EXTRA_OECMAKE += "-DDS_COMRPC=ON"
# ----------------------------------------------------------------
option(DS_COMRPC "Use COM-RPC DeviceSettings client instead of libds/IARM" OFF)

find_package(${NAMESPACE}Plugins REQUIRED)
find_package(${NAMESPACE}Definitions REQUIRED)
find_package(CompileSettingsDebug CONFIG REQUIRED)
find_package(${NAMESPACE}Helpers REQUIRED)

if(NOT DS_COMRPC)
    find_package(DS)
endif()

# ----------------------------------------------------------------
# Plugin shell library — thin layer: lifecycle + JSON-RPC dispatch
# (unchanged regardless of DS_COMRPC setting)
# ----------------------------------------------------------------
add_library(${MODULE_NAME} SHARED
    AudioOutput.cpp
    Module.cpp)

set_target_properties(${MODULE_NAME} PROPERTIES
    CXX_STANDARD 11
    CXX_STANDARD_REQUIRED YES)

target_link_libraries(${MODULE_NAME}
    PRIVATE
        CompileSettingsDebug::CompileSettingsDebug
        ${NAMESPACE}Plugins::${NAMESPACE}Plugins
        ${NAMESPACE}Definitions::${NAMESPACE}Definitions
        ${NAMESPACE}::${NAMESPACE}Helpers)

install(TARGETS ${MODULE_NAME}
    DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/${STORAGE_DIRECTORY}/plugins)

# ----------------------------------------------------------------
# Implementation library — business logic, HAL calls / COM-RPC
#
# DS_COMRPC=ON  -> DS_COMRPC/AudioOutputImplementation.cpp
#                  No libds dependency; uses IDeviceSettingsAudio COM-RPC.
# DS_COMRPC=OFF -> DS_IARM/AudioOutputImplementation.cpp  (legacy, unchanged)
#                  Links against libds / dshal.
# ----------------------------------------------------------------
if(DS_COMRPC)
    message(STATUS "AudioOutput: building DS_COMRPC implementation")

    # DS_COMRPC flag on the plugin shell so AudioOutput.cpp can guard the
    # IConfiguration::Configure(service) call that opens the DSHelper link.
    target_compile_definitions(${MODULE_NAME} PRIVATE DS_COMRPC)

    add_library(${PLUGIN_IMPLEMENTATION} SHARED
        DS_COMRPC/AudioOutputImplementation.cpp
        Module.cpp)

    set_target_properties(${PLUGIN_IMPLEMENTATION} PROPERTIES
        CXX_STANDARD 11
        CXX_STANDARD_REQUIRED YES)

    target_compile_definitions(${PLUGIN_IMPLEMENTATION} PRIVATE DS_COMRPC)

    if(RDK_SERVICE_L2_TEST)
        find_library(TESTMOCKLIB_LIBRARIES NAMES TestMocklib)
        if(TESTMOCKLIB_LIBRARIES)
            message("linking mock libraries ${TESTMOCKLIB_LIBRARIES} library")
            target_link_libraries(${PLUGIN_IMPLEMENTATION} PRIVATE ${TESTMOCKLIB_LIBRARIES})
        else()
            message("Require ${TESTMOCKLIB_LIBRARIES} library")
        endif()
    endif()

    target_link_libraries(${PLUGIN_IMPLEMENTATION}
        PRIVATE
            CompileSettingsDebug::CompileSettingsDebug
            ${NAMESPACE}Plugins::${NAMESPACE}Plugins
            ${NAMESPACE}Definitions::${NAMESPACE}Definitions
            ${NAMESPACE}::${NAMESPACE}Helpers)

else()
    message(STATUS "AudioOutput: building DS_IARM (legacy) implementation")

    add_library(${PLUGIN_IMPLEMENTATION} SHARED
        DS_IARM/AudioOutputImplementation.cpp
        Module.cpp)

    set_target_properties(${PLUGIN_IMPLEMENTATION} PROPERTIES
        CXX_STANDARD 11
        CXX_STANDARD_REQUIRED YES)

    if(RDK_SERVICE_L2_TEST)
        find_library(TESTMOCKLIB_LIBRARIES NAMES TestMocklib)
        if(TESTMOCKLIB_LIBRARIES)
            message("linking mock libraries ${TESTMOCKLIB_LIBRARIES} library")
            target_link_libraries(${PLUGIN_IMPLEMENTATION} PRIVATE ${TESTMOCKLIB_LIBRARIES})
        else()
            message("Require ${TESTMOCKLIB_LIBRARIES} library")
        endif()
    endif()

    target_link_libraries(${PLUGIN_IMPLEMENTATION}
        PRIVATE
            CompileSettingsDebug::CompileSettingsDebug
            ${NAMESPACE}Plugins::${NAMESPACE}Plugins
            ${NAMESPACE}::${NAMESPACE}Helpers)

    if(DS_FOUND)
        target_compile_definitions(${PLUGIN_IMPLEMENTATION} PRIVATE USE_DEVICESETTINGS)
        target_link_libraries(${PLUGIN_IMPLEMENTATION} PRIVATE ${DS_LIBRARIES})
        target_include_directories(${PLUGIN_IMPLEMENTATION} PRIVATE ${DS_INCLUDE_DIRS})
    endif()

endif(DS_COMRPC)

install(TARGETS ${PLUGIN_IMPLEMENTATION}
    DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/${STORAGE_DIRECTORY}/plugins)

write_config(${PLUGIN_NAME})

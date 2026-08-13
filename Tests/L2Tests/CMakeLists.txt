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

set(PLUGIN_NAME L2TestsAudioOutput)
set(MODULE_NAME ${NAMESPACE}${PLUGIN_NAME})
set(THUNDER_PORT 9998)

find_package(${NAMESPACE}Plugins REQUIRED)

# ---------------------------------------------------------------------------
# Source files — guarded by the same CMake option used in the main build
# ---------------------------------------------------------------------------
if(PLUGIN_AUDIOOUTPUT)
    set(SRC_FILES ${SRC_FILES} tests/AudioOutput_L2Test.cpp)
endif()

add_library(${MODULE_NAME} SHARED ${SRC_FILES})

set_target_properties(${MODULE_NAME} PROPERTIES
    CXX_STANDARD 14
    CXX_STANDARD_REQUIRED YES)

target_compile_definitions(${MODULE_NAME}
    PRIVATE
        MODULE_NAME=Plugin_${PLUGIN_NAME}
        THUNDER_PORT="${THUNDER_PORT}")

target_compile_options(${MODULE_NAME} PRIVATE -Wno-error)
target_link_libraries(${MODULE_NAME} PRIVATE ${NAMESPACE}Plugins::${NAMESPACE}Plugins)

# ---------------------------------------------------------------------------
# TestMocklib: stub implementations for devicesettings, IARMBus, RFC, etc.
# Required in in-process (non-OOP) L2 test builds.
# ---------------------------------------------------------------------------
if(NOT L2_TEST_OOP_RPC)
    find_library(TESTMOCKLIB_LIBRARIES NAMES TestMocklib)
    if(TESTMOCKLIB_LIBRARIES)
        message("Found mock library - ${TESTMOCKLIB_LIBRARIES}")
        target_link_libraries(${MODULE_NAME} PRIVATE ${TESTMOCKLIB_LIBRARIES})
    else()
        message("Require ${TESTMOCKLIB_LIBRARIES} library")
    endif()
endif()

# ---------------------------------------------------------------------------
# MockAccessor: provides the global *Impl pointer wiring used by L2TestMocks
# ---------------------------------------------------------------------------
find_library(MOCKACCESSOR_LIBRARIES NAMES MockAccessor)
if(MOCKACCESSOR_LIBRARIES)
    message("Found MockAccessor library - ${MOCKACCESSOR_LIBRARIES}")
    target_link_libraries(${MODULE_NAME} PRIVATE ${MOCKACCESSOR_LIBRARIES})
else()
    message("Require ${MOCKACCESSOR_LIBRARIES} library")
endif()

# ---------------------------------------------------------------------------
# Include paths
# All paths are relative to this CMakeLists.txt location:
#   entservices-audiooutput/Tests/L2Tests/
# ---------------------------------------------------------------------------
target_include_directories(${MODULE_NAME}
    PRIVATE
        # This directory (for any local headers)
        ./

        # Plugin headers — AudioOutputImplementation.h and its DS includes
        ../../plugin

        # testframework: top-level mocks (devicesettings.h, Iarm.h, etc.)
        ../../../entservices-testframework/Tests/mocks

        # testframework: Thunder mocks (ServiceMock.h, COMLinkMock.h)
        ../../../entservices-testframework/Tests/mocks/thunder

        # testframework: devicesettings sub-mocks (HostMock.h, AudioOutputPortMock.h, ...)
        ../../../entservices-testframework/Tests/mocks/devicesettings

        # testframework: MockPlugin helpers
        ../../../entservices-testframework/Tests/mocks/MockPlugin

        # testframework: L2Tests framework (L2TestsMock.h, L2Tests.h, InvokeServiceMethod...)
        ../../../entservices-testframework/Tests/L2Tests/L2TestsPlugin

        # WPEFramework installed headers (Thunder, plugins, interfaces)
        ${CMAKE_INSTALL_PREFIX}/include)

install(TARGETS ${MODULE_NAME} DESTINATION lib)

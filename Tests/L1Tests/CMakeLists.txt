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

cmake_minimum_required(VERSION 3.8)

set(PLUGIN_NAME L1TestsAudioOutput)
set(MODULE_NAME ${NAMESPACE}${PLUGIN_NAME})

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(${NAMESPACE}Plugins REQUIRED)

# ---------------------------------------------------------------------------
# Source and include directories (relative to this CMakeLists.txt location:
#   entservices-audiooutput/Tests/L1Tests/)
# ---------------------------------------------------------------------------
set(PLUGIN_SOURCE_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../../plugin)
# TESTFRAMEWORK_DIR can be overridden via -DTESTFRAMEWORK_DIR=... on the cmake
# command line (used by native/coverity builds where testframework is cloned
# inside GITHUB_WORKSPACE rather than one level above it).
if(NOT DEFINED TESTFRAMEWORK_DIR)
    set(TESTFRAMEWORK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../entservices-testframework)
endif()
set(HELPERS_DIR        ${CMAKE_CURRENT_SOURCE_DIR}/../../../entservices-helpers)

# ---------------------------------------------------------------------------
# Build the L1 test shared library
# ---------------------------------------------------------------------------
add_library(${MODULE_NAME} SHARED
    tests/test_AudioOutput.cpp

    # devicesettings stub — provides the concrete implementations of the
    # device::Host / AudioOutputPort / Manager etc. wrapper methods that
    # delegate to the *Impl pointers (set by setImpl() in tests).
    ${TESTFRAMEWORK_DIR}/Tests/mocks/devicesettings.cpp
)

target_include_directories(${MODULE_NAME}
    PRIVATE
        # Plugin headers — AudioOutputImplementation.h and its DS includes
        ${PLUGIN_SOURCE_DIR}

        # WPEFramework installed headers (Thunder, WPEFramework)
        ${CMAKE_INSTALL_PREFIX}/include/WPEFramework

        # entservices-helpers installed headers
        ${HELPERS_DIR}/helpers

        # testframework: top-level mocks (devicesettings.h, Iarm.h, etc.)
        ${TESTFRAMEWORK_DIR}/Tests/mocks

        # testframework: devicesettings sub-mocks (HostMock.h, AudioOutputPortMock.h, ...)
        ${TESTFRAMEWORK_DIR}/Tests/mocks/devicesettings

        # testframework: Thunder mocks (ServiceMock.h, COMLinkMock.h)
        # Local override first — shadows testframework version to fix Thunder R4_4-RDK
        # incompatibilities (e.g. AddRef return type changed to uint32_t)
        ${CMAKE_CURRENT_SOURCE_DIR}/mocks/thunder
        ${TESTFRAMEWORK_DIR}/Tests/mocks/thunder
)

target_link_directories(${MODULE_NAME}
    PUBLIC
        ${CMAKE_INSTALL_PREFIX}/lib
        ${CMAKE_INSTALL_PREFIX}/lib/wpeframework/plugins
)

target_link_libraries(${MODULE_NAME}
    PRIVATE
        ${NAMESPACE}Plugins::${NAMESPACE}Plugins
        # Plugin implementation libraries (built by cov_build.sh / native build)
        ${NAMESPACE}AudioOutput
        ${NAMESPACE}AudioOutputImplementation
        gmock
        gtest
)

target_compile_definitions(${MODULE_NAME} PRIVATE UNIT_TEST)

install(TARGETS ${MODULE_NAME} DESTINATION lib)

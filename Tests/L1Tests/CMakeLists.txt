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
set(PLUGIN_NAME L1TestsIN)
set(MODULE_NAME ${NAMESPACE}${PLUGIN_NAME})

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(${NAMESPACE}Plugins REQUIRED)

set(TEST_SRC "")
set(TEST_LIB ${NAMESPACE}Plugins::${NAMESPACE}Plugins)
set(TEST_INC ../../helpers)

macro(add_plugin_test_ex plugin_opt plugin_test_sources_str plugin_includes_str plugin_libs_str)
    if(${plugin_opt})
        message(STATUS "${plugin_opt}=ON")
        string(REPLACE ";" ";" srclist "${plugin_test_sources_str}")
        string(REPLACE ";" ";" inclist "${plugin_includes_str}")
        string(REPLACE ";" ";" liblist "${plugin_libs_str}")
        foreach(item IN LISTS srclist)
            list(APPEND TEST_SRC ${item})
        endforeach()
        foreach(item IN LISTS inclist)
            list(APPEND TEST_INC ${item})
        endforeach()
        foreach(item IN LISTS liblist)
            list(APPEND TEST_LIB ${item})
        endforeach()
    else()
        message(STATUS "${plugin_opt}=OFF")
    endif()
endmacro()

# PLUGIN_AUDIOOUTPUT
set(AUDIOOUTPUT_INC
    ${CMAKE_SOURCE_DIR}/../entservices-audiooutput/plugin
    ${CMAKE_SOURCE_DIR}/../entservices-audiooutput/helpers)
set(AUDIOOUTPUT_LIBS ${NAMESPACE}AudioOutput ${NAMESPACE}AudioOutputImplementation)
add_plugin_test_ex(PLUGIN_AUDIOOUTPUT
    tests/test_AudioOutput.cpp
    "${AUDIOOUTPUT_INC}"
    "${AUDIOOUTPUT_LIBS}")

add_library(${MODULE_NAME} SHARED ${TEST_SRC})

include_directories(${TEST_INC})

target_link_directories(${MODULE_NAME} PUBLIC
    ${CMAKE_INSTALL_PREFIX}/lib
    ${CMAKE_INSTALL_PREFIX}/lib/wpeframework/plugins)

target_link_libraries(${MODULE_NAME} ${TEST_LIB})

target_include_directories(${MODULE_NAME}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include>
        ${CMAKE_SOURCE_DIR}/../entservices-testframework/Tests/mocks
        ${CMAKE_SOURCE_DIR}/../entservices-testframework/Tests/mocks/devicesettings
        ${CMAKE_SOURCE_DIR}/../entservices-testframework/Tests/mocks/thunder
        ${CMAKE_SOURCE_DIR}/../Thunder/Source/plugins)

target_compile_definitions(${MODULE_NAME} PRIVATE UNIT_TEST)

install(TARGETS ${MODULE_NAME} DESTINATION lib)
write_config(${PLUGIN_NAME})

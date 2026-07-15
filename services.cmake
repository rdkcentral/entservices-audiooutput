# If not stated otherwise in this file or this component's Licenses.txt file the
# following copyright and licenses apply:
#
# Copyright 2016 RDK Management
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

#
# features
#

add_definitions (-DUSE_IARM)
option(USE_IARM "USE_IARM" ON)

add_definitions (-DUSE_IARM_BUS)
option(USE_IARM_BUS "USE_IARM_BUS" ON)

add_definitions (-DUSE_IARMBUS)

add_definitions (-DUSE_TR_69)
add_definitions (-DHAS_API_SYSTEM)

add_definitions (-DUSE_DS)

option(PLUGIN_TELEMETRY "PLUGIN_TELEMETRY" ON)

add_definitions (-DPLUGIN_CONTINUEWATCHING)
option(PLUGIN_CONTINUEWATCHING "PLUGIN_CONTINUEWATCHING" ON)

if(PLUGIN_CONTINUEWATCHING)
    if(CONTINUEWATCHING_DISABLE_SECAPI)
        add_definitions (-DDISABLE_SECAPI)
    endif()
endif()

if (BUILD_DBUS)
    message("Building for DBUS")

    add_definitions (-DBUILD_DBUS)
    option(BUILD_DBUS "BUILD_DBUS" ON)
    add_definitions (-DIARM_USE_DBUS)
    option(IARM_USE_DBUS "IARM_USE_DBUS" ON)
endif()

if (BUILD_ENABLE_TELEMETRY_LOGGING)
    message("Building with telemetry logging")
    add_definitions (-DENABLE_TELEMETRY_LOGGING)
endif()

if (ENABLE_RFC_MANAGER)
    message("Using binary for RFC Maintenance task")
    add_definitions (-DENABLE_RFC_MANAGER=ON)
endif()

#!/bin/bash
#
# If not stated otherwise in this file or this component's LICENSE
# file the following copyright and licenses apply:
#
# Copyright 2026 RDK Management
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

set -x
set -e
##############################
GITHUB_WORKSPACE="${PWD}"
ls -la ${GITHUB_WORKSPACE}
cd ${GITHUB_WORKSPACE}

# # ############################# 
#1. Install Dependencies and packages

apt update
apt install -y valgrind lcov clang libsystemd-dev meson curl libunwind-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
pip install jsonref

############################
# Build trevor-base64
if [ ! -d "trower-base64" ]; then
git clone https://github.com/xmidt-org/trower-base64.git
fi
cd trower-base64
meson setup --warnlevel 3 --werror build
ninja -C build
ninja -C build install
cd ..
###########################################
# Clone the required repositories


git clone --branch  R4_4-RDK https://github.com/rdkcentral/ThunderTools.git

git clone --branch R4_4-RDK https://github.com/rdkcentral/Thunder.git

git clone --branch 4.1.2 https://github.com/rdkcentral/entservices-apis.git

cd ..
git clone --branch develop https://github.com/rdkcentral/entservices-helpers.git
cd "$GITHUB_WORKSPACE"

git clone --branch 1.0.14 https://github.com/rdkcentral/entservices-testframework.git

# Patch devicesettings.h: testframework 1.0.14 lacks virtual ~IAudioOutputPortEvents(),
# required because the plugin's DsAudioPortNotification uses '~Foo() override = default;'.
grep -q "virtual ~IAudioOutputPortEvents()" entservices-testframework/Tests/mocks/devicesettings.h || \
    sed -i 's/class IAudioOutputPortEvents {/class IAudioOutputPortEvents {\n    public:\n        virtual ~IAudioOutputPortEvents() = default;/' \
        entservices-testframework/Tests/mocks/devicesettings.h

############################
# Build Thunder-Tools
echo "====================================================================================="
echo "building thunderTools"
cd ThunderTools
cd -


cmake -G Ninja -S ThunderTools -B build/ThunderTools \
    -DEXCEPTIONS_ENABLE=ON \
    -DCMAKE_INSTALL_PREFIX="$GITHUB_WORKSPACE/install/usr" \
    -DCMAKE_MODULE_PATH="$GITHUB_WORKSPACE/install/tools/cmake" \
    -DGENERIC_CMAKE_MODULE_PATH="$GITHUB_WORKSPACE/install/tools/cmake" \

cmake --build build/ThunderTools --target install


############################
# Build Thunder
echo "======================================================================================"
echo "buliding thunder"

cd Thunder
cd -

cmake -G Ninja -S Thunder -B build/Thunder \
    -DMESSAGING=ON \
    -DCMAKE_INSTALL_PREFIX="$GITHUB_WORKSPACE/install/usr" \
    -DCMAKE_MODULE_PATH="$GITHUB_WORKSPACE/install/tools/cmake" \
    -DGENERIC_CMAKE_MODULE_PATH="$GITHUB_WORKSPACE/install/tools/cmake" \
    -DBUILD_TYPE=Debug \
    -DBINDING=127.0.0.1 \
    -DPORT=55555 \
    -DEXCEPTIONS_ENABLE=ON \

cmake --build build/Thunder --target install


############################
# Build entservices-apis
echo "======================================================================================"
echo "building entservices-apis"
cd entservices-apis
rm -rf jsonrpc/DTV.json
patch -p1 < $GITHUB_WORKSPACE/entservices-testframework/patches/RDKEMW-1007.patch
cd ..

cmake -G Ninja -S entservices-apis  -B build/entservices-apis \
    -DEXCEPTIONS_ENABLE=ON \
    -DCMAKE_INSTALL_PREFIX="$GITHUB_WORKSPACE/install/usr" \
    -DCMAKE_MODULE_PATH="$GITHUB_WORKSPACE/install/tools/cmake" \

cmake --build build/entservices-apis --target install

############################
# generating minimal mock headers
cd $GITHUB_WORKSPACE/entservices-testframework/Tests
mkdir -p headers
cd headers
touch secure_wrapper.h
touch wpa_ctrl.h
touch rdk_logger_milestone.h
mkdir -p rdk/iarmbus
touch rdk/iarmbus/libIARM.h
touch rdk/iarmbus/libIBus.h
touch iarm.h
cd $GITHUB_WORKSPACE
#########################################
# Build entservices-helpers
echo "======================================================================================"
echo "building entservices-helpers"
cmake -G Ninja -S ../entservices-helpers -B build/entservices-helpers \
    -DEXCEPTIONS_ENABLE=ON \
    -DCMAKE_INSTALL_PREFIX="$GITHUB_WORKSPACE/install/usr" \
    -DCMAKE_MODULE_PATH="$GITHUB_WORKSPACE/install/tools/cmake" \
    -DUSE_THUNDER_R4=ON \
    -DHIDE_NON_EXTERNAL_SYMBOLS=OFF \
    -DPLUGIN_HELPERS=ON \
    "-DCMAKE_CXX_FLAGS=-I$GITHUB_WORKSPACE/entservices-testframework/Tests/mocks -I$GITHUB_WORKSPACE/entservices-testframework/Tests/headers -I$GITHUB_WORKSPACE/entservices-testframework/Tests/headers/rdk/iarmbus -include $GITHUB_WORKSPACE/entservices-testframework/Tests/mocks/Iarm.h " \

cmake --build build/entservices-helpers --target install

############################
# generating extrnal headers
cd $GITHUB_WORKSPACE
cd entservices-testframework/Tests
echo " Empty mocks creation to avoid compilation errors"
echo "======================================================================================"
mkdir -p headers
mkdir -p headers/rdk/ds
echo "dir created successfully"
echo "======================================================================================"

echo "======================================================================================"
echo "empty headers creation"
cd headers
echo "current working dir: "${PWD}
touch rdk/ds/host.hpp
touch rdk/ds/list.hpp
touch rdk/ds/hdmiIn.hpp
touch rdk/ds/manager.hpp
touch rdk/ds/exception.hpp
touch rdk/ds/dsAudio.h
touch rdk/ds/dsError.h
touch rdk/ds/dsMgr.h
touch rdk/ds/dsTypes.h
touch rdk/ds/dsUtl.h
touch rdk/ds/audioOutputPort.hpp
touch rdk/ds/audioOutputPortType.hpp
touch rdk/ds/audioOutputPortConfig.hpp
touch rdk/ds/audioStereoMode.hpp
touch rdk/ds/sleepMode.hpp
touch rdk/ds/videoDevice.hpp
touch rdk/ds/videoOutputPort.hpp
touch rdk/ds/videoOutputPortConfig.hpp
touch rdk/ds/videoOutputPortType.hpp
touch rdk/ds/videoResolution.hpp
touch rdk/ds/audioOutputPortType.hpp
touch rdk/ds/audioOutputPortConfig.hpp
touch rdk/ds/pixelResolution.hpp
touch edid-parser.hpp
echo "files created successfully"
echo "======================================================================================"

ls -la ${GITHUB_WORKSPACE}

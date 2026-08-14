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
############################
# Generate stub headers BEFORE cmake so all -include and -I paths resolve
cd $GITHUB_WORKSPACE/entservices-testframework/Tests
mkdir -p headers \
         headers/audiocapturemgr \
         headers/rdk/ds \
         headers/rdk/iarmbus \
         headers/rdk/iarmmgrs-hal \
         headers/rdk/halif/deepsleep-manager \
         headers/ccec/drivers \
         headers/network \
         headers/proc \
         headers/Dobby/Public/Dobby \
         headers/Dobby/IpcService
cd headers
touch audiocapturemgr/audiocapturemgr_iarm.h
touch ccec/drivers/CecIARMBusMgr.h
touch rdk/ds/audioOutputPort.hpp rdk/ds/audioOutputPortConfig.hpp rdk/ds/audioOutputPortType.hpp
touch rdk/ds/audioStereoMode.hpp rdk/ds/compositeIn.hpp rdk/ds/dsAudio.h
touch rdk/ds/dsDisplay.h rdk/ds/dsError.h rdk/ds/dsMgr.h rdk/ds/dsTypes.h rdk/ds/dsUtl.h
touch rdk/ds/exception.hpp rdk/ds/hdmiIn.hpp rdk/ds/host.hpp rdk/ds/list.hpp
touch rdk/ds/manager.hpp rdk/ds/pixelResolution.hpp rdk/ds/sleepMode.hpp
touch rdk/ds/videoDevice.hpp rdk/ds/videoOutputPort.hpp rdk/ds/videoOutputPortConfig.hpp
touch rdk/ds/videoOutputPortType.hpp rdk/ds/videoResolution.hpp
touch rdk/iarmbus/libIARM.h rdk/iarmbus/libIBus.h rdk/iarmbus/libIBusDaemon.h
touch rdk/halif/deepsleep-manager/deepSleepMgr.h
touch rdk/iarmmgrs-hal/mfrMgr.h rdk/iarmmgrs-hal/sysMgr.h
touch network/wifiSrvMgrIarmIf.h network/netsrvmgrIarm.h
touch Dobby/DobbyProtocol.h Dobby/DobbyProxy.h
touch Dobby/Public/Dobby/IDobbyProxy.h Dobby/IpcService/IpcFactory.h
touch libudev.h rfcapi.h rbus.h telemetry_busmessage_sender.h
touch maintenanceMGR.h pkg.h edid-parser.hpp secure_wrapper.h wpa_ctrl.h
touch proc/readproc.h btmgr.h rdk_logger_milestone.h
cd $GITHUB_WORKSPACE

############################
# Build entservices-audiooutput
echo "building entservices-audiooutput"

cd ${GITHUB_WORKSPACE}
cmake -G Ninja -S "$GITHUB_WORKSPACE" -B build/entservices-audiooutput \
-DUSE_THUNDER_R4=ON \
-DCMAKE_INSTALL_PREFIX="$GITHUB_WORKSPACE/install/usr" \
-DCMAKE_MODULE_PATH="$GITHUB_WORKSPACE/install/tools/cmake" \
-DCMAKE_VERBOSE_MAKEFILE=ON \
-DCMAKE_DISABLE_FIND_PACKAGE_IARMBus=ON \
-DCMAKE_DISABLE_FIND_PACKAGE_RFC=ON \
-DCOMCAST_CONFIG=OFF \
-DRDK_SERVICES_COVERITY=ON \
-DRDK_SERVICES_L1_TEST=ON \
-DDS_FOUND=ON \
-DPLUGIN_AUDIOOUTPUT=ON \
-DTESTFRAMEWORK_DIR="$GITHUB_WORKSPACE/entservices-testframework" \
-DCMAKE_CXX_FLAGS="-DEXCEPTIONS_ENABLE=ON \
-I ${GITHUB_WORKSPACE}/entservices-testframework/Tests/headers \
-I ${GITHUB_WORKSPACE}/entservices-testframework/Tests/headers/audiocapturemgr \
-I ${GITHUB_WORKSPACE}/entservices-testframework/Tests/headers/rdk/ds \
-I ${GITHUB_WORKSPACE}/entservices-testframework/Tests/headers/rdk/iarmbus \
-I ${GITHUB_WORKSPACE}/entservices-testframework/Tests/headers/rdk/iarmmgrs-hal \
-I ${GITHUB_WORKSPACE}/entservices-testframework/Tests/headers/ccec/drivers \
-I ${GITHUB_WORKSPACE}/entservices-testframework/Tests/headers/network \
-I ${GITHUB_WORKSPACE}/entservices-testframework/Tests/headers/libusb \
-I ${GITHUB_WORKSPACE}/entservices-testframework/Tests/headers/Dobby \
-I ${GITHUB_WORKSPACE}/entservices-testframework/Tests/headers/Dobby/Public/Dobby \
-I ${GITHUB_WORKSPACE}/entservices-testframework/Tests/headers/Dobby/IpcService \
-I ${GITHUB_WORKSPACE}/entservices-testframework/Tests/mocks \
-I ${GITHUB_WORKSPACE}/entservices-testframework/Tests/mocks/thunder \
-I /usr/include/libdrm \
-include ${GITHUB_WORKSPACE}/entservices-testframework/Tests/mocks/devicesettings.h \
-include ${GITHUB_WORKSPACE}/entservices-testframework/Tests/mocks/Iarm.h \
-include ${GITHUB_WORKSPACE}/entservices-testframework/Tests/mocks/Rfc.h \
-include ${GITHUB_WORKSPACE}/entservices-testframework/Tests/mocks/RBus.h \
-include ${GITHUB_WORKSPACE}/entservices-testframework/Tests/mocks/Telemetry.h \
-include ${GITHUB_WORKSPACE}/entservices-testframework/Tests/mocks/Udev.h \
-include ${GITHUB_WORKSPACE}/entservices-testframework/Tests/mocks/pkg.h \
-include ${GITHUB_WORKSPACE}/entservices-testframework/Tests/mocks/maintenanceMGR.h \
-include ${GITHUB_WORKSPACE}/entservices-testframework/Tests/mocks/secure_wrappermock.h \
-include ${GITHUB_WORKSPACE}/entservices-testframework/Tests/mocks/gdialservice.h \
-include ${GITHUB_WORKSPACE}/entservices-testframework/Tests/mocks/wpa_ctrl_mock.h \
-include ${GITHUB_WORKSPACE}/entservices-testframework/Tests/mocks/HdmiCec.h \
-include ${GITHUB_WORKSPACE}/entservices-testframework/Tests/mocks/libusb/libusb.h \
-include ${GITHUB_WORKSPACE}/entservices-testframework/Tests/mocks/Dobby.h \
-include ${GITHUB_WORKSPACE}/entservices-testframework/Tests/headers/Dobby/DobbyProtocol.h \
-include ${GITHUB_WORKSPACE}/entservices-testframework/Tests/headers/Dobby/DobbyProxy.h \
-include ${GITHUB_WORKSPACE}/entservices-testframework/Tests/headers/Dobby/Public/Dobby/IDobbyProxy.h \
-include ${GITHUB_WORKSPACE}/entservices-testframework/Tests/headers/Dobby/IpcService/IpcFactory.h \
-Wall -Werror -Wno-error=format \
-Wl,-wrap,system -Wl,-wrap,popen -Wl,-wrap,syslog \
-DENABLE_TELEMETRY_LOGGING -DHAS_API_SYSTEM \
-DHAS_RBUS -DTHUNDER_VERSION=4 -DTHUNDER_VERSION_MAJOR=4 -DTHUNDER_VERSION_MINOR=4" \


cmake --build build/entservices-audiooutput --target install
echo "======================================================================================"

ls -la ${GITHUB_WORKSPACE}
exit 0

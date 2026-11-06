/**
 * If not stated otherwise in this file or this component's LICENSE
 * file the following copyright and licenses apply:
 *
 * Copyright 2025 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "L2Tests.h"
#include "L2TestsMock.h"
#include <mutex>
#include <condition_variable>
#include <memory>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <interfaces/IAudioOutput.h>
#include "devicesettings/DsAudioHALMock.h"
#include "devicesettings/DsHdmiInHALMock.h"

#define TEST_LOG(x, ...) fprintf(stderr, "\033[1;32m[%s:%d](%s)<PID:%d><TID:%d>" x "\n\033[0m", \
    __FILE__, __LINE__, __FUNCTION__, getpid(), gettid(), ##__VA_ARGS__); fflush(stderr);

#define AUDIOOUTPUT_CALLSIGN        _T("org.rdk.AudioOutput")
#define CLEANUP_DELAY_MICROSECONDS  500000
#define DS_SETTLE_DELAY_MICROSECONDS 500000

using namespace WPEFramework;

// ---------------------------------------------------------------------------
// Notification sink to capture OnDolbyAtmosExperienceChanged events
// ---------------------------------------------------------------------------
class AudioOutputNotificationSink : public Exchange::IAudioOutput::INotification {
public:
    AudioOutputNotificationSink()
        : _notified(false)
    {
    }

    void OnDolbyAtmosExperienceChanged(const bool) override
    {
        std::unique_lock<std::mutex> lock(_mutex);
        _notified = true;
        _cv.notify_all();
    }

    bool WaitForNotification(int timeoutMs = 2000)
    {
        std::unique_lock<std::mutex> lock(_mutex);
        return _cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                            [this] { return _notified; });
    }

    bool WasNotified() const { return _notified; }

    BEGIN_INTERFACE_MAP(AudioOutputNotificationSink)
    INTERFACE_ENTRY(Exchange::IAudioOutput::INotification)
    END_INTERFACE_MAP

private:
    mutable std::mutex _mutex;
    std::condition_variable _cv;
    bool _notified;
};

// ---------------------------------------------------------------------------
// Rewrites RDK_PROFILE in /etc/device.properties for the lifetime of the object.
// DeviceSettings and AudioOutput read the profile on each check, so the change is
// visible to the next reload. The original file content is restored on destruction.
// ---------------------------------------------------------------------------
class ScopedRdkProfile {
public:
    explicit ScopedRdkProfile(const std::string& profile)
        : mPath("/etc/device.properties"), mOriginal(), mOriginalRead(false), mActive(false)
    {
        std::ifstream in(mPath);
        if (!in.is_open()) {
            return;
        }
        std::stringstream buffer;
        buffer << in.rdbuf();
        mOriginal = buffer.str();
        mOriginalRead = true;
        in.close();

        std::istringstream lines(mOriginal);
        std::string line;
        std::string rewritten;
        bool replaced = false;
        while (std::getline(lines, line)) {
            if (line.rfind("RDK_PROFILE=", 0) == 0) {
                line = "RDK_PROFILE=" + profile;
                replaced = true;
            }
            rewritten += line + "\n";
        }
        if (!replaced) {
            rewritten += "RDK_PROFILE=" + profile + "\n";
        }
        mActive = Write(rewritten);
    }

    ~ScopedRdkProfile()
    {
        if (mOriginalRead) {
            Write(mOriginal);
        }
    }

    bool IsActive() const { return mActive; }

private:
    bool Write(const std::string& content)
    {
        std::ofstream out(mPath, std::ios::trunc);
        if (!out.is_open()) {
            return false;
        }
        out << content;
        return out.good();
    }

    std::string mPath;
    std::string mOriginal;
    bool mOriginalRead;
    bool mActive;
};

// ---------------------------------------------------------------------------
// L2 test fixture
//
// AudioOutput obtains all audio data from the org.rdk.DeviceSettings plugin over
// COM-RPC. DeviceSettings is activated once per test process, because its HAL
// registration is one-time. The HAL mocks are process-lifetime objects; tests set the
// sink Atmos state via mSinkAtmosCapability and read it back through DolbyAtmosExperience(),
// which evaluates the live HAL state on every call.
// ---------------------------------------------------------------------------
class AudioOutputL2Test : public L2TestMocks {
protected:
    Core::ProxyType<RPC::InvokeServerType<1, 0, 4>> mAudioOutputEngine;
    Core::ProxyType<RPC::CommunicatorClient>        mAudioOutputClient;
    Exchange::IAudioOutput*                          mAudioOutputPlugin{nullptr};

    AudioOutputL2Test();
    virtual ~AudioOutputL2Test() override;

    uint32_t CreateAudioOutputInterfaceObjectUsingComRPCConnection();

    // Process-wide DeviceSettings HAL state. Allocated once and intentionally never freed,
    // because DeviceSettings stays active for the whole test process.
    static ::testing::NiceMock<DsAudioHalMock>* mDsAudioHalMock;
    static ::testing::NiceMock<DsHdmiInHalMock>* mDsHdmiInHalMock;
    static dsAtmosCapsChangeCB_t mAtmosCapsChangeCB;
    static dsHdmiInConnectCB_t mHdmiInConnectCB;
    static dsHdmiInSignalChangeCB_t mHdmiInSignalChangeCB;
    static dsHdmiInStatusChangeCB_t mHdmiInStatusChangeCB;
    static dsHdmiInVideoModeUpdateCB_t mHdmiInVideoModeUpdateCB;
    static dsHdmiInAllmChangeCB_t mHdmiInAllmChangeCB;
    static dsHdmiInAviContentTypeChangeCB_t mHdmiInAviContentTypeChangeCB;
    static dsHdmiInVRRChangeCB_t mHdmiInVRRChangeCB;
    static std::atomic<dsATMOSCapability_t> mSinkAtmosCapability;
    static std::atomic<unsigned int> mSinkAtmosQueryCount;
    static bool mDeviceSettingsActive;

    // Port state overrides read by the HAL mocks. Keys are HAL handles (type + 1 for index 0).
    static std::mutex mAudioPortStateMutex;
    static std::set<intptr_t> mDisabledAudioPorts;
    static std::set<intptr_t> mDisconnectedAudioPorts;
    static std::atomic<intptr_t> mLastAtmosQueryHandle;

    static void ConfigureDeviceSettingsMocks();
    static intptr_t HalAudioPortHandle(dsAudioPortType_t type);
    static void SetAudioPortFlag(std::set<intptr_t>& ports, intptr_t handle, bool present);
};

::testing::NiceMock<DsAudioHalMock>* AudioOutputL2Test::mDsAudioHalMock = nullptr;
::testing::NiceMock<DsHdmiInHalMock>* AudioOutputL2Test::mDsHdmiInHalMock = nullptr;
dsAtmosCapsChangeCB_t AudioOutputL2Test::mAtmosCapsChangeCB = nullptr;
dsHdmiInConnectCB_t AudioOutputL2Test::mHdmiInConnectCB = nullptr;
dsHdmiInSignalChangeCB_t AudioOutputL2Test::mHdmiInSignalChangeCB = nullptr;
dsHdmiInStatusChangeCB_t AudioOutputL2Test::mHdmiInStatusChangeCB = nullptr;
dsHdmiInVideoModeUpdateCB_t AudioOutputL2Test::mHdmiInVideoModeUpdateCB = nullptr;
dsHdmiInAllmChangeCB_t AudioOutputL2Test::mHdmiInAllmChangeCB = nullptr;
dsHdmiInAviContentTypeChangeCB_t AudioOutputL2Test::mHdmiInAviContentTypeChangeCB = nullptr;
dsHdmiInVRRChangeCB_t AudioOutputL2Test::mHdmiInVRRChangeCB = nullptr;
std::atomic<dsATMOSCapability_t> AudioOutputL2Test::mSinkAtmosCapability{dsAUDIO_ATMOS_NOTSUPPORTED};
std::atomic<unsigned int> AudioOutputL2Test::mSinkAtmosQueryCount{0};
bool AudioOutputL2Test::mDeviceSettingsActive = false;
std::mutex AudioOutputL2Test::mAudioPortStateMutex;
std::set<intptr_t> AudioOutputL2Test::mDisabledAudioPorts;
std::set<intptr_t> AudioOutputL2Test::mDisconnectedAudioPorts;
std::atomic<intptr_t> AudioOutputL2Test::mLastAtmosQueryHandle{0};

AudioOutputL2Test::AudioOutputL2Test()
    : L2TestMocks()
{
    TEST_LOG("AudioOutputL2Test constructor");

    // Process-wide DeviceSettings HAL mocks: configured once, reused by every test
    if (mDsAudioHalMock == nullptr) {
        ConfigureDeviceSettingsMocks();
    }
    DsAudioApi::setImpl(mDsAudioHalMock);
    DsHdmiInApi::setImpl(mDsHdmiInHalMock);

    // DeviceSettings is activated once per process so the HDMI-In connect callback registers once
    if (!mDeviceSettingsActive) {
        uint32_t dsStatus = ActivateServiceWithRetry(_T("org.rdk.DeviceSettings"), 3, 500);
        EXPECT_EQ(Core::ERROR_NONE, dsStatus) << "Failed to activate DeviceSettings plugin";
        mDeviceSettingsActive = (dsStatus == Core::ERROR_NONE);
    }

    // Reset per-test sink state before AudioOutput reads it on activation
    mSinkAtmosCapability = dsAUDIO_ATMOS_NOTSUPPORTED;
    mSinkAtmosQueryCount = 0;
    mLastAtmosQueryHandle = 0;
    {
        std::lock_guard<std::mutex> lock(mAudioPortStateMutex);
        mDisabledAudioPorts.clear();
        mDisconnectedAudioPorts.clear();
    }

    uint32_t status = ActivateService(AUDIOOUTPUT_CALLSIGN);
    EXPECT_EQ(Core::ERROR_NONE, status) << "Failed to activate AudioOutput plugin";
}

AudioOutputL2Test::~AudioOutputL2Test()
{
    TEST_LOG("AudioOutputL2Test destructor");

    if (mAudioOutputPlugin != nullptr) {
        mAudioOutputPlugin->Release();
        mAudioOutputPlugin = nullptr;
    }
    usleep(CLEANUP_DELAY_MICROSECONDS);
    DeactivateService(AUDIOOUTPUT_CALLSIGN);

    // DeviceSettings stays active for the process; the base fixture clears its HAL impls
}

void AudioOutputL2Test::ConfigureDeviceSettingsMocks()
{
    using ::testing::_;
    using ::testing::Invoke;
    using ::testing::Return;

    mDsAudioHalMock = new ::testing::NiceMock<DsAudioHalMock>();
    mDsHdmiInHalMock = new ::testing::NiceMock<DsHdmiInHalMock>();

    // HDMI-In: DeviceSettings registers these callbacks during its activation
    ON_CALL(*mDsHdmiInHalMock, dsHdmiInInit()).WillByDefault(Return(dsERR_NONE));
    ON_CALL(*mDsHdmiInHalMock, dsHdmiInTerm()).WillByDefault(Return(dsERR_NONE));
    ON_CALL(*mDsHdmiInHalMock, dsHdmiInRegisterConnectCB(_))
        .WillByDefault(Invoke([](dsHdmiInConnectCB_t cbFun) {
            mHdmiInConnectCB = cbFun;
            return dsERR_NONE;
        }));

    ON_CALL(*mDsHdmiInHalMock, dsHdmiInRegisterSignalChangeCB(_))
        .WillByDefault(Invoke([](dsHdmiInSignalChangeCB_t cbFun) {
            mHdmiInSignalChangeCB = cbFun;
            return dsERR_NONE;
        }));
    ON_CALL(*mDsHdmiInHalMock, dsHdmiInRegisterStatusChangeCB(_))
        .WillByDefault(Invoke([](dsHdmiInStatusChangeCB_t cbFun) {
            mHdmiInStatusChangeCB = cbFun;
            return dsERR_NONE;
        }));
    ON_CALL(*mDsHdmiInHalMock, dsHdmiInRegisterVideoModeUpdateCB(_))
        .WillByDefault(Invoke([](dsHdmiInVideoModeUpdateCB_t cbFun) {
            mHdmiInVideoModeUpdateCB = cbFun;
            return dsERR_NONE;
        }));
    ON_CALL(*mDsHdmiInHalMock, dsHdmiInRegisterAllmChangeCB(_))
        .WillByDefault(Invoke([](dsHdmiInAllmChangeCB_t cbFun) {
            mHdmiInAllmChangeCB = cbFun;
            return dsERR_NONE;
        }));
    ON_CALL(*mDsHdmiInHalMock, dsHdmiInRegisterAviContentTypeChangeCB(_))
        .WillByDefault(Invoke([](dsHdmiInAviContentTypeChangeCB_t cbFun) {
            mHdmiInAviContentTypeChangeCB = cbFun;
            return dsERR_NONE;
        }));
    ON_CALL(*mDsHdmiInHalMock, dsHdmiInRegisterVRRChangeCB(_))
        .WillByDefault(Invoke([](dsHdmiInVRRChangeCB_t cbFun) {
            mHdmiInVRRChangeCB = cbFun;
            return dsERR_NONE;
        }));

    // Port handles are non-zero for index 0 only, so each port type resolves to a distinct handle
    ON_CALL(*mDsAudioHalMock, dsGetAudioPort(_, _, _))
        .WillByDefault(Invoke([](dsAudioPortType_t type, int index, intptr_t* handle) {
            if (index != 0 || handle == nullptr) {
                return dsERR_OPERATION_NOT_SUPPORTED;
            }
            *handle = static_cast<intptr_t>(type) + 1;
            return dsERR_NONE;
        }));

    ON_CALL(*mDsAudioHalMock, dsIsAudioPortEnabled(_, _))
        .WillByDefault(Invoke([](intptr_t handle, bool* enabled) {
            if (enabled) {
                std::lock_guard<std::mutex> lock(mAudioPortStateMutex);
                *enabled = mDisabledAudioPorts.count(handle) == 0;
            }
            return dsERR_NONE;
        }));

    ON_CALL(*mDsAudioHalMock, dsAudioOutIsConnected(_, _))
        .WillByDefault(Invoke([](intptr_t handle, bool* isConnected) {
            if (isConnected) {
                std::lock_guard<std::mutex> lock(mAudioPortStateMutex);
                *isConnected = mDisconnectedAudioPorts.count(handle) == 0;
            }
            return dsERR_NONE;
        }));

    ON_CALL(*mDsAudioHalMock, dsGetStereoMode(_, _))
        .WillByDefault(Invoke([](intptr_t, dsAudioStereoMode_t* mode) {
            if (mode) {
                *mode = dsAUDIO_STEREO_PASSTHRU;
            }
            return dsERR_NONE;
        }));

    ON_CALL(*mDsAudioHalMock, dsGetStereoAuto(_, _))
        .WillByDefault(Invoke([](intptr_t, int* autoMode) {
            if (autoMode) {
                *autoMode = 0;
            }
            return dsERR_NONE;
        }));

    // Sink Atmos capability comes from mSinkAtmosCapability; every read is counted
    ON_CALL(*mDsAudioHalMock, dsGetSinkDeviceAtmosCapability(_, _))
        .WillByDefault(Invoke([](intptr_t handle, dsATMOSCapability_t* capability) {
            ++mSinkAtmosQueryCount;
            mLastAtmosQueryHandle = handle;
            if (capability) {
                *capability = mSinkAtmosCapability.load();
            }
            return dsERR_NONE;
        }));

    // Capture the Atmos callback so tests can fire it
    ON_CALL(*mDsAudioHalMock, dsAudioAtmosCapsChangeRegisterCB(_))
        .WillByDefault(Invoke([](dsAtmosCapsChangeCB_t cbFun) {
            mAtmosCapsChangeCB = cbFun;
            return dsERR_NONE;
        }));
}

uint32_t AudioOutputL2Test::CreateAudioOutputInterfaceObjectUsingComRPCConnection()
{
    mAudioOutputEngine = Core::ProxyType<RPC::InvokeServerType<1, 0, 4>>::Create();
    mAudioOutputClient = Core::ProxyType<RPC::CommunicatorClient>::Create(
        Core::NodeId(_T("/tmp/communicator")),
        Core::ProxyType<Core::IIPCServer>(mAudioOutputEngine));

    if (!mAudioOutputClient.IsValid()) {
        TEST_LOG("Failed to create CommunicatorClient");
        return Core::ERROR_GENERAL;
    }

    PluginHost::IShell* shell = mAudioOutputClient->Open<PluginHost::IShell>(
        AUDIOOUTPUT_CALLSIGN, ~0, 3000);

    if (shell == nullptr) {
        TEST_LOG("Failed to acquire IShell interface");
        return Core::ERROR_GENERAL;
    }

    mAudioOutputPlugin = shell->QueryInterface<Exchange::IAudioOutput>();
    shell->Release();

    if (mAudioOutputPlugin != nullptr) {
        TEST_LOG("Successfully acquired IAudioOutput interface via COM-RPC");
        return Core::ERROR_NONE;
    }

    TEST_LOG("Failed to acquire IAudioOutput interface");
    return Core::ERROR_GENERAL;
}

// ===========================================================================
// JSON-RPC and COM-RPC return the default value (false) when DeviceSettings
// has not provided any Atmos capability / sound mode.
// ===========================================================================
TEST_F(AudioOutputL2Test, JsonRpc_DolbyAtmosExperience_DefaultFalse_WhenNoAtmosCapability)
{
    JsonObject params, result;
    uint32_t status = InvokeServiceMethod(
        AUDIOOUTPUT_CALLSIGN,
        "dolbyAtmosExperience",
        params,
        result);

    EXPECT_EQ(Core::ERROR_NONE, status) << "JSON-RPC call must succeed";

    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());
    ASSERT_NE(mAudioOutputPlugin, nullptr);

    bool value = true; // pre-set true to confirm it gets overwritten to false
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->DolbyAtmosExperience(value));
    EXPECT_FALSE(value) << "No DeviceSettings data: dolbyAtmosExperience must be false";
}

// ===========================================================================
// DolbyAtmosExperience() is a pure getter: repeated calls must be stable and
// must not trigger notifications.
// ===========================================================================
TEST_F(AudioOutputL2Test, ComRpc_NoNotification_WhenGetterCalledMultipleTimes)
{
    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());
    ASSERT_NE(mAudioOutputPlugin, nullptr);

    auto sink = Core::ProxyType<AudioOutputNotificationSink>::Create();
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->Register(&(*sink)));

    bool v1 = true, v2 = true, v3 = true;
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->DolbyAtmosExperience(v1));
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->DolbyAtmosExperience(v2));
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->DolbyAtmosExperience(v3));

    EXPECT_FALSE(v1);
    EXPECT_EQ(v1, v2);
    EXPECT_EQ(v2, v3);

    EXPECT_FALSE(sink->WaitForNotification(500)) << "Getter must never fire notifications";
    EXPECT_FALSE(sink->WasNotified());

    mAudioOutputPlugin->Unregister(&(*sink));
}

// ===========================================================================
// Register / Unregister edge cases
// ===========================================================================

// Registering the same notification twice is tolerated (second call is a no-op).
TEST_F(AudioOutputL2Test, ComRpc_Register_Duplicate_ReturnsOk)
{
    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());
    ASSERT_NE(mAudioOutputPlugin, nullptr);

    auto sink = Core::ProxyType<AudioOutputNotificationSink>::Create();
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->Register(&(*sink)));
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->Register(&(*sink)));

    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->Unregister(&(*sink)));
}

// Unregistering a notification that was never registered must not crash.
TEST_F(AudioOutputL2Test, ComRpc_Unregister_NotFound_ReturnsOk)
{
    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());
    ASSERT_NE(mAudioOutputPlugin, nullptr);

    auto sink = Core::ProxyType<AudioOutputNotificationSink>::Create();
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->Unregister(&(*sink)));
}

// ===========================================================================
// Sink reports Atmos METADATA: the getter reads the live HAL state, and a false-to-true
// transition must be announced to registered observers.
// ===========================================================================
TEST_F(AudioOutputL2Test, ComRpc_NotifiesTrue_WhenAtmosMetadataCapabilityReported)
{
    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());
    ASSERT_NE(mAudioOutputPlugin, nullptr);

    auto sink = Core::ProxyType<AudioOutputNotificationSink>::Create();
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->Register(&(*sink)));

    // Baseline: no Atmos reported, so the first read is false and nothing is announced
    bool enabled = true;
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->DolbyAtmosExperience(enabled));
    EXPECT_FALSE(enabled);
    EXPECT_FALSE(sink->WaitForNotification(500)) << "Baseline read must not notify";

    mSinkAtmosCapability = dsAUDIO_ATMOS_ATMOSMETADATA;

    enabled = false;
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->DolbyAtmosExperience(enabled));
    EXPECT_TRUE(enabled) << "Atmos METADATA with passthrough sound mode must report true";
    EXPECT_TRUE(sink->WaitForNotification()) << "Atmos false->true transition must notify observers";

    mAudioOutputPlugin->Unregister(&(*sink));
}

// ===========================================================================
// Sink reports Atmos NOT_SUPPORTED: no notification and the getter stays false.
// ===========================================================================
TEST_F(AudioOutputL2Test, ComRpc_NoNotification_WhenAtmosNotSupportedReported)
{
    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());
    ASSERT_NE(mAudioOutputPlugin, nullptr);

    auto sink = Core::ProxyType<AudioOutputNotificationSink>::Create();
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->Register(&(*sink)));

    mSinkAtmosCapability = dsAUDIO_ATMOS_NOTSUPPORTED;

    bool enabled = true;
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->DolbyAtmosExperience(enabled));
    EXPECT_FALSE(enabled) << "NOT_SUPPORTED Atmos must report false";
    EXPECT_FALSE(sink->WaitForNotification(500)) << "Unchanged Atmos state must not notify";

    mAudioOutputPlugin->Unregister(&(*sink));
}

// ===========================================================================
// The getter has no cache: every call re-reads the sink capability from the HAL,
// so a change in the sink is visible on the next call without any event.
// ===========================================================================
TEST_F(AudioOutputL2Test, Getter_ReflectsLiveSinkCapability_NoCache)
{
    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());
    ASSERT_NE(mAudioOutputPlugin, nullptr);

    mSinkAtmosCapability = dsAUDIO_ATMOS_ATMOSMETADATA;
    unsigned int queriesBefore = mSinkAtmosQueryCount.load();

    bool enabled = false;
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->DolbyAtmosExperience(enabled));
    EXPECT_TRUE(enabled) << "Sink reports Atmos: getter must return true";
    EXPECT_GT(mSinkAtmosQueryCount.load(), queriesBefore) << "Getter must query the HAL on each call";

    mSinkAtmosCapability = dsAUDIO_ATMOS_NOTSUPPORTED;
    queriesBefore = mSinkAtmosQueryCount.load();

    enabled = true;
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->DolbyAtmosExperience(enabled));
    EXPECT_FALSE(enabled) << "Sink capability change must be visible on the next call";
    EXPECT_GT(mSinkAtmosQueryCount.load(), queriesBefore) << "Getter must query the HAL on each call";
}

// ===========================================================================
// Helpers for HAL port state and STB profile tests
// ===========================================================================
intptr_t AudioOutputL2Test::HalAudioPortHandle(dsAudioPortType_t type)
{
    return static_cast<intptr_t>(type) + 1;
}

void AudioOutputL2Test::SetAudioPortFlag(std::set<intptr_t>& ports, intptr_t handle, bool present)
{
    std::lock_guard<std::mutex> lock(mAudioPortStateMutex);
    if (present) {
        ports.insert(handle);
    } else {
        ports.erase(handle);
    }
}

// ===========================================================================
// AtmosMetadata STB branch (RDK_PROFILE=STB). The STB branch queries HDMI0 when it is
// connected and never queries HDMI_ARC0. The profile is rewritten for the test and restored on exit.
// ===========================================================================
TEST_F(AudioOutputL2Test, AtmosMetadata_Stb_ArcEnabled_DoesNotQueryArcPort)
{
    ScopedRdkProfile profile("STB");
    ASSERT_TRUE(profile.IsActive()) << "/etc/device.properties must be writable to set RDK_PROFILE=STB";

    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());
    ASSERT_NE(mAudioOutputPlugin, nullptr);
    usleep(DS_SETTLE_DELAY_MICROSECONDS);

    mSinkAtmosCapability = dsAUDIO_ATMOS_ATMOSMETADATA;
    mLastAtmosQueryHandle = 0;

    bool enabled = false;
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->DolbyAtmosExperience(enabled));
    EXPECT_EQ(HalAudioPortHandle(dsAUDIOPORT_TYPE_HDMI), mLastAtmosQueryHandle.load())
        << "STB with HDMI0 connected must query HDMI0";
    EXPECT_NE(HalAudioPortHandle(dsAUDIOPORT_TYPE_HDMI_ARC), mLastAtmosQueryHandle.load())
        << "STB must not query HDMI_ARC0";
}

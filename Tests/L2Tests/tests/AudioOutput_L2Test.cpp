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
// L2 test fixture
//
// AudioOutput obtains all audio data from the org.rdk.DeviceSettings plugin over
// COM-RPC. DeviceSettings is activated once per test process, because its HDMI-In
// hot plug HAL registration is one-time. The HAL mocks are process-lifetime objects;
// tests set the sink Atmos state via mSinkAtmosCapability and fire the captured
// Atmos and HDMI-In callbacks to inject events.
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
    static std::atomic<dsATMOSCapability_t> mSinkAtmosCapability;
    static std::atomic<unsigned int> mSinkAtmosQueryCount;
    static bool mDeviceSettingsActive;

    static void ConfigureDeviceSettingsMocks();
    static void FireHdmiInHotPlug(bool connected);
};

::testing::NiceMock<DsAudioHalMock>* AudioOutputL2Test::mDsAudioHalMock = nullptr;
::testing::NiceMock<DsHdmiInHalMock>* AudioOutputL2Test::mDsHdmiInHalMock = nullptr;
dsAtmosCapsChangeCB_t AudioOutputL2Test::mAtmosCapsChangeCB = nullptr;
dsHdmiInConnectCB_t AudioOutputL2Test::mHdmiInConnectCB = nullptr;
std::atomic<dsATMOSCapability_t> AudioOutputL2Test::mSinkAtmosCapability{dsAUDIO_ATMOS_NOTSUPPORTED};
std::atomic<unsigned int> AudioOutputL2Test::mSinkAtmosQueryCount{0};
bool AudioOutputL2Test::mDeviceSettingsActive = false;

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

    // HDMI-In: capture the connect callback so tests can inject hot plug events
    ON_CALL(*mDsHdmiInHalMock, dsHdmiInInit()).WillByDefault(Return(dsERR_NONE));
    ON_CALL(*mDsHdmiInHalMock, dsHdmiInTerm()).WillByDefault(Return(dsERR_NONE));
    ON_CALL(*mDsHdmiInHalMock, dsHdmiInRegisterConnectCB(_))
        .WillByDefault(Invoke([](dsHdmiInConnectCB_t cbFun) {
            mHdmiInConnectCB = cbFun;
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
        .WillByDefault(Invoke([](intptr_t, bool* enabled) {
            if (enabled) {
                *enabled = true;
            }
            return dsERR_NONE;
        }));

    ON_CALL(*mDsAudioHalMock, dsAudioOutIsConnected(_, _))
        .WillByDefault(Invoke([](intptr_t, bool* isConnected) {
            if (isConnected) {
                *isConnected = true;
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
        .WillByDefault(Invoke([](intptr_t, dsATMOSCapability_t* capability) {
            ++mSinkAtmosQueryCount;
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
// DeviceSettings reports Atmos METADATA: AudioOutput must notify and report true.
// ===========================================================================
TEST_F(AudioOutputL2Test, ComRpc_NotifiesTrue_WhenAtmosMetadataCapabilityReported)
{
    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());
    ASSERT_NE(mAudioOutputPlugin, nullptr);
    ASSERT_NE(mAtmosCapsChangeCB, nullptr) << "DeviceSettings did not register the Atmos callback";

    auto sink = Core::ProxyType<AudioOutputNotificationSink>::Create();
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->Register(&(*sink)));

    // DeviceSettings initialises SoundMode asynchronously after activation
    usleep(DS_SETTLE_DELAY_MICROSECONDS);

    mAtmosCapsChangeCB(dsAUDIO_ATMOS_ATMOSMETADATA, true);

    EXPECT_TRUE(sink->WaitForNotification()) << "Atmos METADATA event must notify observers";

    bool enabled = false;
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->DolbyAtmosExperience(enabled));
    EXPECT_TRUE(enabled) << "Atmos METADATA with passthrough sound mode must report true";

    mAudioOutputPlugin->Unregister(&(*sink));
}

// ===========================================================================
// DeviceSettings reports Atmos NOT_SUPPORTED: no notification and getter stays false.
// ===========================================================================
TEST_F(AudioOutputL2Test, ComRpc_NoNotification_WhenAtmosNotSupportedReported)
{
    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());
    ASSERT_NE(mAudioOutputPlugin, nullptr);
    ASSERT_NE(mAtmosCapsChangeCB, nullptr) << "DeviceSettings did not register the Atmos callback";

    auto sink = Core::ProxyType<AudioOutputNotificationSink>::Create();
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->Register(&(*sink)));

    mAtmosCapsChangeCB(dsAUDIO_ATMOS_NOTSUPPORTED, false);

    EXPECT_FALSE(sink->WaitForNotification(500)) << "Unchanged Atmos state must not notify";

    bool enabled = true;
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->DolbyAtmosExperience(enabled));
    EXPECT_FALSE(enabled) << "NOT_SUPPORTED Atmos must report false";

    mAudioOutputPlugin->Unregister(&(*sink));
}

// ===========================================================================
// HDMI-In hot plug: AudioOutput marks its Atmos cache for reload and re-reads
// the sink capability on the next DolbyAtmosExperience() call.
// ===========================================================================
void AudioOutputL2Test::FireHdmiInHotPlug(bool connected)
{
    ASSERT_NE(mHdmiInConnectCB, nullptr) << "DeviceSettings did not register the HDMI-In connect callback";
    mHdmiInConnectCB(static_cast<dsHdmiInPort_t>(0), connected);
    usleep(DS_SETTLE_DELAY_MICROSECONDS);
}

TEST_F(AudioOutputL2Test, HdmiInHotPlug_Connected_ReloadsAtmosCapabilityOnNextRead)
{
    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());
    ASSERT_NE(mAudioOutputPlugin, nullptr);

    // Let the activation-time initialisation finish before changing the sink state
    usleep(DS_SETTLE_DELAY_MICROSECONDS);

    auto sink = Core::ProxyType<AudioOutputNotificationSink>::Create();
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->Register(&(*sink)));

    bool before = true;
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->DolbyAtmosExperience(before));
    EXPECT_FALSE(before) << "Sink reports no Atmos before the hot plug";

    mSinkAtmosCapability = dsAUDIO_ATMOS_ATMOSMETADATA;
    FireHdmiInHotPlug(true);

    bool enabled = false;
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->DolbyAtmosExperience(enabled));
    EXPECT_TRUE(enabled) << "Connected HDMI-In must re-read the sink Atmos capability";
    EXPECT_TRUE(sink->WaitForNotification(500)) << "Changed Atmos state must notify observers";

    mAudioOutputPlugin->Unregister(&(*sink));
}

TEST_F(AudioOutputL2Test, HdmiInHotPlug_Disconnected_ReloadsAtmosCapabilityOnNextRead)
{
    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());
    ASSERT_NE(mAudioOutputPlugin, nullptr);
    usleep(DS_SETTLE_DELAY_MICROSECONDS);

    // Establish a cached Atmos=true state through a connect event
    mSinkAtmosCapability = dsAUDIO_ATMOS_ATMOSMETADATA;
    FireHdmiInHotPlug(true);
    bool enabled = false;
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->DolbyAtmosExperience(enabled));
    ASSERT_TRUE(enabled) << "Precondition: connected sink must report Atmos";

    // Sink unplugged: the disconnect event must drop the cached value on the next read
    mSinkAtmosCapability = dsAUDIO_ATMOS_NOTSUPPORTED;
    FireHdmiInHotPlug(false);

    enabled = true;
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->DolbyAtmosExperience(enabled));
    EXPECT_FALSE(enabled) << "Disconnected HDMI-In must re-read the sink and clear Atmos";
}

TEST_F(AudioOutputL2Test, HdmiInHotPlug_NoEvent_KeepsCachedAtmosCapability)
{
    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());
    ASSERT_NE(mAudioOutputPlugin, nullptr);
    usleep(DS_SETTLE_DELAY_MICROSECONDS);

    mSinkAtmosCapability = dsAUDIO_ATMOS_ATMOSMETADATA;
    FireHdmiInHotPlug(true);
    bool enabled = false;
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->DolbyAtmosExperience(enabled));
    ASSERT_TRUE(enabled) << "Precondition: connected sink must report Atmos";

    // HAL state changes without a hot plug event: the cache must be served with no new query
    mSinkAtmosCapability = dsAUDIO_ATMOS_NOTSUPPORTED;
    const unsigned int queriesBefore = mSinkAtmosQueryCount.load();

    enabled = false;
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->DolbyAtmosExperience(enabled));
    EXPECT_TRUE(enabled) << "Without a hot plug event the cached Atmos value must be kept";
    EXPECT_EQ(queriesBefore, mSinkAtmosQueryCount.load()) << "No HAL query expected without a hot plug event";
}

TEST_F(AudioOutputL2Test, HdmiInHotPlug_RepeatedEvents_QueryOnceOnRead)
{
    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());
    ASSERT_NE(mAudioOutputPlugin, nullptr);
    usleep(DS_SETTLE_DELAY_MICROSECONDS);

    mSinkAtmosCapability = dsAUDIO_ATMOS_ATMOSMETADATA;
    const unsigned int queriesBefore = mSinkAtmosQueryCount.load();

    FireHdmiInHotPlug(true);
    FireHdmiInHotPlug(false);
    FireHdmiInHotPlug(true);

    bool first = false;
    bool second = false;
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->DolbyAtmosExperience(first));
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->DolbyAtmosExperience(second));
    EXPECT_TRUE(first);
    EXPECT_TRUE(second);
    EXPECT_EQ(queriesBefore + 1, mSinkAtmosQueryCount.load()) << "Coalesced hot plug events must trigger a single HAL re-read";
}

TEST_F(AudioOutputL2Test, HdmiInHotPlug_UnchangedAtmos_DoesNotNotify)
{
    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());
    ASSERT_NE(mAudioOutputPlugin, nullptr);
    usleep(DS_SETTLE_DELAY_MICROSECONDS);

    mSinkAtmosCapability = dsAUDIO_ATMOS_ATMOSMETADATA;
    FireHdmiInHotPlug(true);
    bool enabled = false;
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->DolbyAtmosExperience(enabled));
    ASSERT_TRUE(enabled) << "Precondition: connected sink must report Atmos";

    auto sink = Core::ProxyType<AudioOutputNotificationSink>::Create();
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->Register(&(*sink)));

    // Same Atmos state after the event: re-read, but the value must not be re-announced
    FireHdmiInHotPlug(true);
    enabled = false;
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->DolbyAtmosExperience(enabled));
    EXPECT_TRUE(enabled);
    EXPECT_FALSE(sink->WaitForNotification(500)) << "Unchanged Atmos value must not notify";

    mAudioOutputPlugin->Unregister(&(*sink));
}

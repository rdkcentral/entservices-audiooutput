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
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <list>
#include <mutex>
#include <string>
#include <future>
#include <vector>

#include "AudioOutputImplementation.h"

// Thunder mocks (ServiceMock, WorkerPoolImplementation)
#include "ServiceMock.h"
#include "WorkerPoolImplementation.h"

// DeviceSettings COM-RPC mocks from entservices-testframework
#include "DeviceSettingsMock.h"

#define TEST_LOG(x, ...) \
    fprintf(stderr, "\033[1;32m[%s:%d](%s)<PID:%d><TID:%d>" x "\n\033[0m", \
        __FILE__, __LINE__, __FUNCTION__, getpid(), gettid(), ##__VA_ARGS__); \
    fflush(stderr);

using ::testing::_;
using ::testing::DoAll;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::SetArgReferee;
using namespace WPEFramework;

namespace {

using Audio              = Exchange::IDeviceSettingsAudio;
using AudioPort          = Audio::AudioPortType;
using StereoMode         = Audio::StereoMode;
using AtmosCap           = Audio::DolbyAtmosCapability;
using HdmiInIface        = Exchange::IDeviceSettingsHDMIIn;
using PortStatusIterator = HdmiInIface::IHDMIInPortConnectionStatusIterator;

const std::string kDsCallsign  = "org.rdk.DeviceSettings";

// Audio port handles handed out by the mocked IDeviceSettingsAudio::GetAudioPort().
constexpr int32_t kHdmiHandle      = 10;
constexpr int32_t kArcHandle       = 11;
constexpr int32_t kSpeakerHandle   = 12;
constexpr int32_t kSpdifHandle     = 13;
constexpr int32_t kHeadphoneHandle = 14;
// Video port handle of HDMI0 (DSHelper uses it to check HDMI display connectivity).
constexpr int32_t kVideoHdmiHandle = 20;

// Minimal notification mock to capture OnDolbyAtmosExperienceChanged calls
class MockAudioOutputNotification : public Exchange::IAudioOutput::INotification {
public:
    MOCK_METHOD(void, OnDolbyAtmosExperienceChanged, (const bool dolbyAtmosExperience), (override));

    BEGIN_INTERFACE_MAP(MockAudioOutputNotification)
    INTERFACE_ENTRY(Exchange::IAudioOutput::INotification)
    END_INTERFACE_MAP
};

// DSHelper activates the implementation asynchronously on the worker pool; this
// subclass signals once OnDeviceSettingsActivated() has finished.
class TestableAudioOutputImplementation : public Plugin::AudioOutputImplementation {
public:
    void OnDeviceSettingsActivated() override
    {
        Plugin::AudioOutputImplementation::OnDeviceSettingsActivated();
        std::lock_guard<std::mutex> lock(_mutex);
        ++_activations;
        _cv.notify_all();
    }

    bool WaitForActivations(unsigned int count, unsigned int timeoutMs = 5000)
    {
        std::unique_lock<std::mutex> lock(_mutex);
        return _cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                            [&] { return _activations >= count; });
    }

private:
    std::mutex _mutex;
    std::condition_variable _cv;
    unsigned int _activations{0};
};

// Completion job for the test worker pool. The pool runs a single thread and dispatches
// FIFO, so when this job has run every job submitted before it has finished.
class SyncDispatchJob : public Core::IDispatch {
public:
    explicit SyncDispatchJob(std::promise<void>& done)
        : _done(done)
    {
    }
    ~SyncDispatchJob() = default;

    void Dispatch() override
    {
        _done.set_value();
    }

private:
    std::promise<void>& _done;
};

// Blocks until the plugin's pending worker-pool jobs (change-notification dispatch) have run.
void DrainWorkerPool()
{
    std::promise<void> done;
    std::future<void> finished = done.get_future();
    Core::IWorkerPool::Instance().Submit(
        Core::ProxyType<Core::IDispatch>(Core::ProxyType<SyncDispatchJob>::Create(done)));
    ASSERT_EQ(std::future_status::ready, finished.wait_for(std::chrono::seconds(5)))
        << "worker pool did not drain";
}

} // namespace

// ---------------------------------------------------------------------------
// Test fixture
//
// The plugin talks to the DeviceSettings plugin over COM-RPC through DSHelper.
// The fixture emulates an already active org.rdk.DeviceSettings plugin:
//   - ServiceMock::Register(IPlugin::INotification*) immediately reports the
//     DeviceSettings callsign as activated, so DSHelper calls
//     OnDeviceSettingsActivated().
//   - ServiceMock::QueryInterface(IDeviceSettings::ID) returns DeviceSettingsMock
//     whose sub-interface mocks (Audio, VideoPort, HDMIIn) the tests configure.
// ---------------------------------------------------------------------------
class AudioOutputImplementationTest : public ::testing::Test {
protected:
    NiceMock<ServiceMock> service;

    DeviceSettingsMock*          dsMock        = nullptr;
    DeviceSettingsAudioMock*     audioMock     = nullptr;
    DeviceSettingsVideoPortMock* videoPortMock = nullptr;
    DeviceSettingsHDMIInMock*    hdmiInMock    = nullptr;

    Core::ProxyType<WorkerPoolImplementation>          workerPool;
    Core::ProxyType<TestableAudioOutputImplementation> impl;

    // Captured callback: DSHelper plugin monitor
    PluginHost::IPlugin::INotification* dsPluginSink = nullptr;

    void SetUp() override
    {
        // WorkerPoolImplementation creates (threads - 1) workers, so 2 gives exactly one worker thread.
        // Jobs then run in submission order, which DrainWorkerPool() relies on.
        workerPool = Core::ProxyType<WorkerPoolImplementation>::Create(2, Core::Thread::DefaultStackSize(), 16);
        Core::IWorkerPool::Assign(&(*workerPool));
        workerPool->Run();

        dsMock        = &DeviceSettingsMock::Mock();
        audioMock     = &DeviceSettingsAudioMock::Mock();
        videoPortMock = &DeviceSettingsVideoPortMock::Mock();
        hdmiInMock    = &DeviceSettingsHDMIInMock::Mock();

        // DSHelper registers an IPlugin::INotification on the shell. Report DeviceSettings
        // as already active from inside Register(), exactly like Thunder does.
        ON_CALL(service, Register(::testing::A<PluginHost::IPlugin::INotification*>()))
            .WillByDefault([this](PluginHost::IPlugin::INotification* sink) {
                dsPluginSink = sink;
                sink->Activated(kDsCallsign, &service);
            });
        ON_CALL(service, QueryInterface(::testing::Eq(static_cast<uint32_t>(Exchange::IDeviceSettings::ID))))
            .WillByDefault([](const uint32_t) -> void* {
                Exchange::IDeviceSettings* root = DeviceSettingsMock::Get();
                root->AddRef();
                return static_cast<void*>(root);
            });

        // Static DeviceSettings configuration: HDMI0, HDMI_ARC0, SPEAKER0, SPDIF0, HEADPHONE0
        ON_CALL(*dsMock, GetDeviceSettingConfigs(_))
            .WillByDefault([](Exchange::IDeviceSettings::DeviceSettingConfigs& configs) {
                configs.audioPorts = {
                    { static_cast<int32_t>(Audio::AUDIO_PORT_TYPE_HDMI),      0, 0, 0 },
                    { static_cast<int32_t>(Audio::AUDIO_PORT_TYPE_HDMIARC),   0, 0, 0 },
                    { static_cast<int32_t>(Audio::AUDIO_PORT_TYPE_SPEAKER),   0, 0, 0 },
                    { static_cast<int32_t>(Audio::AUDIO_PORT_TYPE_SPDIF),     0, 0, 0 },
                    { static_cast<int32_t>(Audio::AUDIO_PORT_TYPE_HEADPHONE), 0, 0, 0 },
                };
                Exchange::IDeviceSettings::VideoPortPortConfig hdmi;
                hdmi.videoPortType = static_cast<int32_t>(Exchange::IDeviceSettingsVideoPort::DS_VIDEO_PORT_TYPE_HDMI);
                hdmi.videoPortIndex = 0;
                hdmi.connectedAudioPortType = static_cast<int32_t>(Audio::AUDIO_PORT_TYPE_HDMI);
                hdmi.connectedAudioPortIndex = 0;
                hdmi.defaultResolution = "720p";
                configs.videoPorts = { hdmi };
                return static_cast<Core::hresult>(Core::ERROR_NONE);
            });
        ON_CALL(*audioMock, GetAudioPort(_, _, _))
            .WillByDefault([](const AudioPort type, const int32_t, int32_t& handle) {
                switch (type) {
                case Audio::AUDIO_PORT_TYPE_HDMI:      handle = kHdmiHandle;      break;
                case Audio::AUDIO_PORT_TYPE_HDMIARC:   handle = kArcHandle;       break;
                case Audio::AUDIO_PORT_TYPE_SPEAKER:   handle = kSpeakerHandle;   break;
                case Audio::AUDIO_PORT_TYPE_SPDIF:     handle = kSpdifHandle;     break;
                case Audio::AUDIO_PORT_TYPE_HEADPHONE: handle = kHeadphoneHandle; break;
                default: return static_cast<Core::hresult>(Core::ERROR_GENERAL);
                }
                return static_cast<Core::hresult>(Core::ERROR_NONE);
            });
        ON_CALL(*videoPortMock, GetVideoPort(_, _, _))
            .WillByDefault(DoAll(SetArgReferee<2>(kVideoHdmiHandle), Return(Core::ERROR_NONE)));

        // The HDMI display is connected unless a test says otherwise.
        SetHdmiDisplayConnected(true);

        impl = Core::ProxyType<TestableAudioOutputImplementation>::Create();
    }

    void TearDown() override
    {
        DrainWorkerPool();
        impl = Core::ProxyType<TestableAudioOutputImplementation>();

        dsPluginSink = nullptr;

        DeviceSettingsMock::Delete();

        Core::IWorkerPool::Assign(nullptr);
        workerPool->Stop();
        workerPool = Core::ProxyType<WorkerPoolImplementation>();
    }

    // ------------------------------------------------------------------
    // Port state helpers. STB profile precedence:
    //   HDMI_ARC > HDMI > SPEAKER > SPDIF > HEADPHONE
    // Ports are disabled unless a test enables them.
    // ------------------------------------------------------------------
    void SetPortEnabled(int32_t handle, bool enabled)
    {
        ON_CALL(*audioMock, IsAudioPortEnabled(handle, _))
            .WillByDefault(DoAll(SetArgReferee<1>(enabled), Return(Core::ERROR_NONE)));
    }

    void SetStereoMode(int32_t handle, StereoMode mode)
    {
        ON_CALL(*audioMock, GetStereoMode(handle, _, _))
            .WillByDefault(DoAll(SetArgReferee<1>(mode), Return(Core::ERROR_NONE)));
    }

    void SetStereoAuto(int32_t handle, int32_t autoMode)
    {
        ON_CALL(*audioMock, GetStereoAuto(handle, _))
            .WillByDefault(DoAll(SetArgReferee<1>(autoMode), Return(Core::ERROR_NONE)));
    }

    void SetAtmosCapability(int32_t handle, AtmosCap capability)
    {
        ON_CALL(*audioMock, GetAudioSinkDeviceAtmosCapability(handle, _))
            .WillByDefault(DoAll(SetArgReferee<1>(capability), Return(Core::ERROR_NONE)));
    }

    void SetHdmiDisplayConnected(bool connected)
    {
        ON_CALL(*videoPortMock, IsVideoPortDisplayConnected(kVideoHdmiHandle, _))
            .WillByDefault(DoAll(SetArgReferee<1>(connected), Return(Core::ERROR_NONE)));
    }

    // HDMI_ARC0 is connected when the HDMI-In port returned by GetAudioHDMIARCPortId() is connected.
    void SetArcConnected(bool connected)
    {
        ON_CALL(*audioMock, GetAudioHDMIARCPortId(kArcHandle, _))
            .WillByDefault(DoAll(SetArgReferee<1>(1), Return(Core::ERROR_NONE)));
        ON_CALL(*hdmiInMock, GetHDMIInStatus(_, _))
            .WillByDefault([connected](HdmiInIface::HDMIInStatus&, PortStatusIterator*& iterator) {
                std::list<HdmiInIface::HDMIPortConnectionStatus> ports{ { false }, { connected } };
                iterator = Core::Service<RPC::IteratorType<PortStatusIterator>>::Create<PortStatusIterator>(ports);
                return static_cast<Core::hresult>(Core::ERROR_NONE);
            });
    }

    void SetHeadphoneConnected(bool connected)
    {
        ON_CALL(*audioMock, IsAudioOutputConnected(kHeadphoneHandle, _))
            .WillByDefault(DoAll(SetArgReferee<1>(connected), Return(Core::ERROR_NONE)));
    }

    // Configure() opens the DSHelper link; activation is processed asynchronously.
    void ConfigureAndWait(unsigned int activations = 1)
    {
        ASSERT_EQ(Core::ERROR_NONE, impl->Configure(&service));
        ASSERT_TRUE(impl->WaitForActivations(activations)) << "DeviceSettings activation was not processed";
        DrainWorkerPool(); // flush any pending notification dispatch
    }

    bool DolbyAtmosExperience()
    {
        bool enabled = false;
        EXPECT_EQ(Core::ERROR_NONE, impl->DolbyAtmosExperience(enabled));
        return enabled;
    }

    // HDMI0 is the only enabled port and its passthrough stereo mode selects it as the sound mode.
    void SetupHdmiPassthrough(AtmosCap capability)
    {
        SetPortEnabled(kHdmiHandle, true);
        SetAtmosCapability(kHdmiHandle, capability);
        SetStereoMode(kHdmiHandle, Audio::AUDIO_STEREO_PASSTHROUGH);
    }
};

// ===========================================================================
// Tests: construction without DeviceSettings
// ===========================================================================

// Before DeviceSettings is activated the value is unavailable; the caller's flag is untouched.
TEST_F(AudioOutputImplementationTest, DolbyAtmosExperience_BeforeDeviceSettingsActivation_ReturnsError)
{
    bool enabled = true; // must not be modified on error
    EXPECT_EQ(Core::ERROR_GENERAL, impl->DolbyAtmosExperience(enabled));
    EXPECT_TRUE(enabled);
}

// The value is computed from live DeviceSettings state on every call (no cached HDMI-In/CEC/audio state).
TEST_F(AudioOutputImplementationTest, DolbyAtmosExperience_ReadsLiveStateOnEveryCall)
{
    TEST_LOG("Atmos capability and sound mode are re-read on each getter call");

    ConfigureAndWait();
    SetupHdmiPassthrough(Audio::AUDIO_DOLBY_ATMOS_NOT_SUPPORTED);
    EXPECT_FALSE(DolbyAtmosExperience());

    SetAtmosCapability(kHdmiHandle, Audio::AUDIO_DOLBY_ATMOS_METADATA);
    EXPECT_TRUE(DolbyAtmosExperience());

    SetStereoMode(kHdmiHandle, Audio::AUDIO_STEREO_STEREO);
    EXPECT_FALSE(DolbyAtmosExperience());
}

TEST_F(AudioOutputImplementationTest, InterfaceMap_QueryInterface_AudioOutput)
{
    auto* iface = static_cast<Exchange::IAudioOutput*>(impl->QueryInterface(Exchange::IAudioOutput::ID));
    ASSERT_NE(iface, nullptr) << "QueryInterface(IAudioOutput::ID) must return non-null";
    iface->Release();
}

TEST_F(AudioOutputImplementationTest, InterfaceMap_QueryInterface_Configuration)
{
    auto* iface = static_cast<Exchange::IConfiguration*>(impl->QueryInterface(Exchange::IConfiguration::ID));
    ASSERT_NE(iface, nullptr) << "QueryInterface(IConfiguration::ID) must return non-null";
    iface->Release();
}

// ===========================================================================
// Tests: Configure() / DeviceSettings activation
// ===========================================================================

// Nothing enabled: HDMI0 is queried (display connected) and reports NOT_SUPPORTED; no sound mode port.
TEST_F(AudioOutputImplementationTest, Configure_NoPortsEnabled_ReturnsFalse)
{
    TEST_LOG("Configure: no enabled ports");

    ConfigureAndWait();
    EXPECT_FALSE(DolbyAtmosExperience());
}

// HDMI0 enabled+connected, Atmos metadata + PASSTHROUGH at activation.
TEST_F(AudioOutputImplementationTest, Configure_HdmiAtmosPassthrough_ReturnsTrue)
{
    TEST_LOG("Configure: HDMI0 ATMOS_METADATA + PASSTHROUGH -> true");

    SetPortEnabled(kHdmiHandle, true);
    SetAtmosCapability(kHdmiHandle, Audio::AUDIO_DOLBY_ATMOS_METADATA);
    SetStereoMode(kHdmiHandle, Audio::AUDIO_STEREO_PASSTHROUGH);

    ConfigureAndWait();
    EXPECT_TRUE(DolbyAtmosExperience());
}

// HDMI0 enabled but the display is not connected -> HDMI0 is skipped for the sound mode,
// and the Atmos capability is read from handle 0 (no HDMI0 display to query).
TEST_F(AudioOutputImplementationTest, Configure_HdmiDisplayNotConnected_SkipsHdmiSoundMode)
{
    TEST_LOG("Configure: HDMI0 not connected -> SPEAKER0 sound mode is used");

    SetHdmiDisplayConnected(false);
    SetPortEnabled(kHdmiHandle, true);
    SetPortEnabled(kSpeakerHandle, true);
    SetAtmosCapability(0, Audio::AUDIO_DOLBY_ATMOS_METADATA);
    SetStereoMode(kHdmiHandle, Audio::AUDIO_STEREO_MONO);
    SetStereoMode(kSpeakerHandle, Audio::AUDIO_STEREO_PASSTHROUGH);

    ConfigureAndWait();
    EXPECT_TRUE(DolbyAtmosExperience()) << "SPEAKER0 PASSTHROUGH must be used when HDMI0 is not connected";
}

// HDMI_ARC > HDMI precedence for the sound mode. The Atmos capability is read from HDMI0.
TEST_F(AudioOutputImplementationTest, Configure_ArcEnabled_ArcTakesPrecedenceOverHdmiSoundMode)
{
    TEST_LOG("Configure: HDMI_ARC0 sound mode preferred over HDMI0");

    SetPortEnabled(kArcHandle, true);
    SetPortEnabled(kHdmiHandle, true);
    SetArcConnected(true);
    SetAtmosCapability(kHdmiHandle, Audio::AUDIO_DOLBY_ATMOS_METADATA);
    SetStereoMode(kArcHandle, Audio::AUDIO_STEREO_DDPLUS);
    SetStereoMode(kHdmiHandle, Audio::AUDIO_STEREO_MONO);

    ConfigureAndWait();
    EXPECT_TRUE(DolbyAtmosExperience()) << "HDMI_ARC0 DDPLUS must be selected over HDMI0 MONO";
}

// HDMI_ARC enabled but not connected -> next port in the precedence list (HDMI) is used.
TEST_F(AudioOutputImplementationTest, Configure_ArcNotConnected_UsesHdmiSoundMode)
{
    TEST_LOG("Configure: HDMI_ARC0 not connected -> HDMI0 sound mode");

    SetPortEnabled(kArcHandle, true);
    SetPortEnabled(kHdmiHandle, true);
    SetArcConnected(false);
    SetAtmosCapability(kHdmiHandle, Audio::AUDIO_DOLBY_ATMOS_METADATA);
    SetStereoMode(kArcHandle, Audio::AUDIO_STEREO_MONO);
    SetStereoMode(kHdmiHandle, Audio::AUDIO_STEREO_DDPLUS);

    ConfigureAndWait();
    EXPECT_TRUE(DolbyAtmosExperience()) << "HDMI0 DDPLUS must be used when ARC is not connected";
}

// ARC/SPDIF with stereo auto -> SOUNDMODE_AUTO, which also enables Atmos.
TEST_F(AudioOutputImplementationTest, Configure_ArcStereoAuto_ReturnsTrue)
{
    TEST_LOG("Configure: HDMI_ARC0 stereo auto -> SOUNDMODE_AUTO");

    SetPortEnabled(kArcHandle, true);
    SetArcConnected(true);
    SetAtmosCapability(kHdmiHandle, Audio::AUDIO_DOLBY_ATMOS_METADATA);
    SetStereoMode(kArcHandle, Audio::AUDIO_STEREO_STEREO);
    SetStereoAuto(kArcHandle, 1);

    ConfigureAndWait();
    EXPECT_TRUE(DolbyAtmosExperience());
}

// HDMI never uses the stereo auto check: STEREO stays STEREO -> false.
TEST_F(AudioOutputImplementationTest, Configure_HdmiStereoAuto_IsIgnored)
{
    TEST_LOG("Configure: HDMI0 ignores stereo auto");

    SetPortEnabled(kHdmiHandle, true);
    SetAtmosCapability(kHdmiHandle, Audio::AUDIO_DOLBY_ATMOS_METADATA);
    SetStereoMode(kHdmiHandle, Audio::AUDIO_STEREO_STEREO);
    SetStereoAuto(kHdmiHandle, 1);

    EXPECT_CALL(*audioMock, GetStereoAuto(kHdmiHandle, _)).Times(0);

    ConfigureAndWait();
    EXPECT_FALSE(DolbyAtmosExperience());
}

// SPEAKER0 only: the STB Atmos capability is read from HDMI0 only, never from SPEAKER0.
TEST_F(AudioOutputImplementationTest, Configure_SpeakerOnly_AtmosCapabilityNotReadFromSpeaker)
{
    TEST_LOG("Configure: only SPEAKER0 enabled -> no Atmos query on SPEAKER0");

    SetPortEnabled(kSpeakerHandle, true);
    SetAtmosCapability(kSpeakerHandle, Audio::AUDIO_DOLBY_ATMOS_METADATA);
    SetStereoMode(kSpeakerHandle, Audio::AUDIO_STEREO_SURROUND);

    EXPECT_CALL(*audioMock, GetAudioSinkDeviceAtmosCapability(kSpeakerHandle, _)).Times(0);

    ConfigureAndWait();
    EXPECT_FALSE(DolbyAtmosExperience()) << "SPEAKER0 SURROUND selects the sound mode but must not enable Atmos";
}

// SPDIF is considered always connected; stereo auto selects SOUNDMODE_AUTO.
TEST_F(AudioOutputImplementationTest, Configure_SpdifPort_StereoAuto_ReturnsTrue)
{
    TEST_LOG("Configure: SPDIF0 stereo auto -> SOUNDMODE_AUTO");

    SetPortEnabled(kSpdifHandle, true);
    SetAtmosCapability(kHdmiHandle, Audio::AUDIO_DOLBY_ATMOS_METADATA);
    SetStereoMode(kSpdifHandle, Audio::AUDIO_STEREO_STEREO);
    SetStereoAuto(kSpdifHandle, 1);

    ConfigureAndWait();
    EXPECT_TRUE(DolbyAtmosExperience());
}

// HEADPHONE is connected only when IsAudioOutputConnected() says so.
TEST_F(AudioOutputImplementationTest, Configure_HeadphoneConnected_UsesHeadphoneSoundMode)
{
    TEST_LOG("Configure: HEADPHONE0 connected");

    SetPortEnabled(kHeadphoneHandle, true);
    SetHeadphoneConnected(true);
    SetAtmosCapability(kHdmiHandle, Audio::AUDIO_DOLBY_ATMOS_METADATA);
    SetStereoMode(kHeadphoneHandle, Audio::AUDIO_STEREO_PASSTHROUGH);

    ConfigureAndWait();
    EXPECT_TRUE(DolbyAtmosExperience());
}

TEST_F(AudioOutputImplementationTest, Configure_HeadphoneNotConnected_IsSkipped)
{
    TEST_LOG("Configure: HEADPHONE0 not connected");

    SetPortEnabled(kHeadphoneHandle, true);
    SetHeadphoneConnected(false);
    SetAtmosCapability(kHdmiHandle, Audio::AUDIO_DOLBY_ATMOS_METADATA);
    SetStereoMode(kHeadphoneHandle, Audio::AUDIO_STEREO_PASSTHROUGH);

    ConfigureAndWait();
    EXPECT_FALSE(DolbyAtmosExperience());
}

// A port whose GetStereoMode() fails is skipped; the next port in precedence wins.
TEST_F(AudioOutputImplementationTest, Configure_GetStereoModeFails_FallsThroughToNextPort)
{
    TEST_LOG("Configure: HDMI0 GetStereoMode fails -> SPEAKER0 used");

    SetPortEnabled(kHdmiHandle, true);
    SetPortEnabled(kSpeakerHandle, true);
    SetAtmosCapability(kHdmiHandle, Audio::AUDIO_DOLBY_ATMOS_METADATA);
    ON_CALL(*audioMock, GetStereoMode(kHdmiHandle, _, _)).WillByDefault(Return(Core::ERROR_GENERAL));
    SetStereoMode(kSpeakerHandle, Audio::AUDIO_STEREO_PASSTHROUGH);

    ConfigureAndWait();
    EXPECT_TRUE(DolbyAtmosExperience());
}

// Atmos capability other than ATMOS_METADATA is not Atmos capable.
TEST_F(AudioOutputImplementationTest, Configure_DdPlusStreamCapability_IsNotAtmosMetadata)
{
    TEST_LOG("Configure: DDPLUS_STREAM capability -> false");

    SetPortEnabled(kHdmiHandle, true);
    SetAtmosCapability(kHdmiHandle, Audio::AUDIO_DOLBY_ATMOS_DDPLUS_STREAM);
    SetStereoMode(kHdmiHandle, Audio::AUDIO_STEREO_PASSTHROUGH);

    ConfigureAndWait();
    EXPECT_FALSE(DolbyAtmosExperience());
}

// DeviceSettings cannot hand out audio port handles -> all queries fail gracefully.
TEST_F(AudioOutputImplementationTest, Configure_GetAudioPortFails_ReturnsFalse)
{
    TEST_LOG("Configure: GetAudioPort fails for every port");

    ON_CALL(*audioMock, GetAudioPort(_, _, _)).WillByDefault(Return(Core::ERROR_GENERAL));
    SetPortEnabled(kHdmiHandle, true);
    SetAtmosCapability(kHdmiHandle, Audio::AUDIO_DOLBY_ATMOS_METADATA);
    SetStereoMode(kHdmiHandle, Audio::AUDIO_STEREO_PASSTHROUGH);

    ConfigureAndWait();
    EXPECT_FALSE(DolbyAtmosExperience());
}

TEST_F(AudioOutputImplementationTest, Configure_GetDeviceSettingConfigsFails_ReturnsFalse)
{
    TEST_LOG("Configure: GetDeviceSettingConfigs fails");

    ON_CALL(*dsMock, GetDeviceSettingConfigs(_)).WillByDefault(Return(Core::ERROR_GENERAL));
    SetPortEnabled(kHdmiHandle, true);
    SetAtmosCapability(kHdmiHandle, Audio::AUDIO_DOLBY_ATMOS_METADATA);
    SetStereoMode(kHdmiHandle, Audio::AUDIO_STEREO_PASSTHROUGH);

    ConfigureAndWait();
    EXPECT_FALSE(DolbyAtmosExperience());
}

// ===========================================================================
// Tests: notifications
// ===========================================================================

// false -> true transition fires notification with value=true
TEST_F(AudioOutputImplementationTest, Notification_FiredOnFalseToTrueTransition)
{
    TEST_LOG("Notification fires on false->true transition");

    ConfigureAndWait();
    SetupHdmiPassthrough(Audio::AUDIO_DOLBY_ATMOS_NOT_SUPPORTED);
    ASSERT_FALSE(DolbyAtmosExperience());

    auto notification = Core::ProxyType<MockAudioOutputNotification>::Create();
    impl->Register(&(*notification));

    EXPECT_CALL(*notification, OnDolbyAtmosExperienceChanged(true)).Times(1);

    SetAtmosCapability(kHdmiHandle, Audio::AUDIO_DOLBY_ATMOS_METADATA); // false -> true
    EXPECT_TRUE(DolbyAtmosExperience());
    DrainWorkerPool();

    impl->Unregister(&(*notification));
}

// true -> false transition fires notification with value=false
TEST_F(AudioOutputImplementationTest, Notification_FiredOnTrueToFalseTransition)
{
    TEST_LOG("Notification fires on true->false transition");

    ConfigureAndWait();
    SetupHdmiPassthrough(Audio::AUDIO_DOLBY_ATMOS_METADATA);
    ASSERT_TRUE(DolbyAtmosExperience());
    DrainWorkerPool(); // flush the true transition before registering

    auto notification = Core::ProxyType<MockAudioOutputNotification>::Create();
    impl->Register(&(*notification));

    EXPECT_CALL(*notification, OnDolbyAtmosExperienceChanged(false)).Times(1);

    SetStereoMode(kHdmiHandle, Audio::AUDIO_STEREO_STEREO); // true -> false
    EXPECT_FALSE(DolbyAtmosExperience());
    DrainWorkerPool();

    impl->Unregister(&(*notification));
}

// No change of the computed value -> no notification
TEST_F(AudioOutputImplementationTest, Notification_NotFired_WhenValueStaysFalse)
{
    TEST_LOG("No notification while the computed value stays false");

    ConfigureAndWait();
    SetupHdmiPassthrough(Audio::AUDIO_DOLBY_ATMOS_NOT_SUPPORTED);

    auto notification = Core::ProxyType<MockAudioOutputNotification>::Create();
    impl->Register(&(*notification));

    EXPECT_CALL(*notification, OnDolbyAtmosExperienceChanged(_)).Times(0);

    SetStereoMode(kHdmiHandle, Audio::AUDIO_STEREO_MONO);
    EXPECT_FALSE(DolbyAtmosExperience());
    SetStereoMode(kHdmiHandle, Audio::AUDIO_STEREO_PASSTHROUGH); // Atmos not capable
    EXPECT_FALSE(DolbyAtmosExperience());
    SetAtmosCapability(kHdmiHandle, Audio::AUDIO_DOLBY_ATMOS_DDPLUS_STREAM);
    EXPECT_FALSE(DolbyAtmosExperience());
    DrainWorkerPool();

    impl->Unregister(&(*notification));
}

// Same value repeated -> notification fires only once
TEST_F(AudioOutputImplementationTest, Notification_FiredOnce_WhenEnablingModeRepeated)
{
    TEST_LOG("Notification fires only once for repeated enabling events");

    ConfigureAndWait();
    SetupHdmiPassthrough(Audio::AUDIO_DOLBY_ATMOS_NOT_SUPPORTED);

    auto notification = Core::ProxyType<MockAudioOutputNotification>::Create();
    impl->Register(&(*notification));

    EXPECT_CALL(*notification, OnDolbyAtmosExperienceChanged(true)).Times(1);

    SetAtmosCapability(kHdmiHandle, Audio::AUDIO_DOLBY_ATMOS_METADATA);
    EXPECT_TRUE(DolbyAtmosExperience());
    EXPECT_TRUE(DolbyAtmosExperience()); // no-op
    SetStereoMode(kHdmiHandle, Audio::AUDIO_STEREO_DDPLUS); // enabling -> enabling
    EXPECT_TRUE(DolbyAtmosExperience());
    DrainWorkerPool();

    impl->Unregister(&(*notification));
}

TEST_F(AudioOutputImplementationTest, Notification_NotFired_AfterUnregister)
{
    TEST_LOG("No notification after Unregister");

    ConfigureAndWait();
    SetupHdmiPassthrough(Audio::AUDIO_DOLBY_ATMOS_NOT_SUPPORTED);

    auto notification = Core::ProxyType<MockAudioOutputNotification>::Create();
    impl->Register(&(*notification));
    impl->Unregister(&(*notification));

    EXPECT_CALL(*notification, OnDolbyAtmosExperienceChanged(_)).Times(0);

    SetAtmosCapability(kHdmiHandle, Audio::AUDIO_DOLBY_ATMOS_METADATA);
    EXPECT_TRUE(DolbyAtmosExperience());
    DrainWorkerPool();
}

// ===========================================================================
// Tests: Register / Unregister edge cases
// ===========================================================================

// Register the same notification pointer twice -> second call logs an error but
// does NOT add a duplicate and returns ERROR_NONE.
TEST_F(AudioOutputImplementationTest, Register_DuplicateNotification_DeliveredOnce)
{
    TEST_LOG("Register same notification twice -> delivered once");

    ConfigureAndWait();
    SetupHdmiPassthrough(Audio::AUDIO_DOLBY_ATMOS_NOT_SUPPORTED);

    auto notification = Core::ProxyType<MockAudioOutputNotification>::Create();
    EXPECT_EQ(Core::ERROR_NONE, impl->Register(&(*notification)));
    EXPECT_EQ(Core::ERROR_NONE, impl->Register(&(*notification)));

    EXPECT_CALL(*notification, OnDolbyAtmosExperienceChanged(true)).Times(1);

    SetAtmosCapability(kHdmiHandle, Audio::AUDIO_DOLBY_ATMOS_METADATA);
    EXPECT_TRUE(DolbyAtmosExperience());
    DrainWorkerPool();

    impl->Unregister(&(*notification));
}

TEST_F(AudioOutputImplementationTest, Unregister_NotFound_ReturnsNone)
{
    TEST_LOG("Unregister notification that was never registered");

    auto notification = Core::ProxyType<MockAudioOutputNotification>::Create();
    EXPECT_EQ(Core::ERROR_NONE, impl->Unregister(&(*notification)));
}

// ===========================================================================
// Tests: DeviceSettings plugin lifecycle (deactivation / re-activation / teardown)
// ===========================================================================

TEST_F(AudioOutputImplementationTest, Reactivation_ReevaluatesLiveStateAfterDeviceSettingsRestart)
{
    TEST_LOG("DeviceSettings restart -> value unavailable while down, live state re-evaluated after reactivation");

    ConfigureAndWait();
    ASSERT_NE(dsPluginSink, nullptr);
    ASSERT_FALSE(DolbyAtmosExperience());

    // DeviceSettings goes away: the value is unavailable.
    dsPluginSink->Deactivated(kDsCallsign, &service);
    bool enabled = false;
    EXPECT_EQ(Core::ERROR_GENERAL, impl->DolbyAtmosExperience(enabled));

    // DeviceSettings comes back with different state.
    SetPortEnabled(kHdmiHandle, true);
    SetAtmosCapability(kHdmiHandle, Audio::AUDIO_DOLBY_ATMOS_METADATA);
    SetStereoMode(kHdmiHandle, Audio::AUDIO_STEREO_PASSTHROUGH);

    dsPluginSink->Activated(kDsCallsign, &service);
    ASSERT_TRUE(impl->WaitForActivations(2));
    DrainWorkerPool();

    EXPECT_TRUE(DolbyAtmosExperience());
}

TEST_F(AudioOutputImplementationTest, Destructor_UnregistersDeviceSettingsNotifications)
{
    TEST_LOG("Destructor unregisters the DeviceSettings plugin notification");

    ConfigureAndWait();

    EXPECT_CALL(service, Unregister(::testing::A<PluginHost::IPlugin::INotification*>())).Times(1);

    impl = Core::ProxyType<TestableAudioOutputImplementation>();
}


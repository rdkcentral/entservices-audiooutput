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
#include <interfaces/IAudioOutput.h>

// NOTE: HostMock.h, AudioOutputPortMock.h, AudioOutputPortTypeMock.h are
// already included transitively via L2TestsMock.h above.

#define TEST_LOG(x, ...) fprintf(stderr, "\033[1;32m[%s:%d](%s)<PID:%d><TID:%d>" x "\n\033[0m", \
    __FILE__, __LINE__, __FUNCTION__, getpid(), gettid(), ##__VA_ARGS__); fflush(stderr);

#define JSON_TIMEOUT                (1000)
#define AUDIOOUTPUT_CALLSIGN        _T("org.rdk.AudioOutput")
#define AUDIOOUTPUT_L2TEST_CALLSIGN _T("L2tests.1")
#define CLEANUP_DELAY_MICROSECONDS  500000

using ::testing::NiceMock;
using ::testing::_;
using ::testing::Return;
using ::testing::ReturnRef;
using ::testing::ReturnRefOfCopy;
using ::testing::SaveArg;
using ::testing::DoAll;
using ::testing::A;
using ::testing::Throw;
using namespace WPEFramework;

// ---------------------------------------------------------------------------
// Notification sink to capture OnDolbyAtmosExperienceChanged events
// ---------------------------------------------------------------------------
class AudioOutputNotificationSink : public Exchange::IAudioOutput::INotification {
public:
    AudioOutputNotificationSink()
        : _notified(false)
        , _lastValue(false)
    {
    }

    void OnDolbyAtmosExperienceChanged(const bool dolbyAtmosExperience) override
    {
        std::unique_lock<std::mutex> lock(_mutex);
        _lastValue = dolbyAtmosExperience;
        _notified = true;
        _cv.notify_all();
    }

    bool WaitForNotification(int timeoutMs = 2000)
    {
        std::unique_lock<std::mutex> lock(_mutex);
        return _cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                            [this] { return _notified; });
    }

    bool LastValue() const { return _lastValue; }
    bool WasNotified() const { return _notified; }
    void Reset() { _notified = false; }

    BEGIN_INTERFACE_MAP(AudioOutputNotificationSink)
    INTERFACE_ENTRY(Exchange::IAudioOutput::INotification)
    END_INTERFACE_MAP

private:
    mutable std::mutex _mutex;
    std::condition_variable _cv;
    bool _notified;
    bool _lastValue;
};

// ---------------------------------------------------------------------------
// L2 Test base fixture
//
// The plugin keeps no cache: DolbyAtmosExperience() queries the DS HAL mocks on
// every call and never notifies. Notifications come from the DS audio events,
// HDMI-In hot plug and CEC power status, which re-query the HAL and notify only
// when the derived value changes. Tests drive state by changing the mock return
// values (SetAtmosCapability / SetSoundMode) and firing the captured listeners.
// The CEC path needs an HdmiCecSink plugin and is covered by the L1 tests.
// ---------------------------------------------------------------------------
class AudioOutputL2Test : public L2TestMocks {
protected:
    // Listeners the plugin registers during construction; captured by the Host::Register ON_CALLs.
    device::Host::IAudioOutputPortEvents* dsListener{nullptr};
    device::Host::IHdmiInEvents*          hdmiListener{nullptr};

    // Persistent object returned by getAudioOutputPort(); calls delegate to p_audioOutputPortMock.
    device::AudioOutputPort _audioPortObj;
    // Persistent object whose getId() delegates to p_audioOutputPortTypeMock.
    device::AudioOutputPortType portTypeObj;

    Core::ProxyType<RPC::InvokeServerType<1, 0, 4>> mAudioOutputEngine;
    Core::ProxyType<RPC::CommunicatorClient>        mAudioOutputClient;
    Exchange::IAudioOutput*                          mAudioOutputPlugin{nullptr};

    AudioOutputL2Test();
    virtual ~AudioOutputL2Test() override;

    uint32_t CreateAudioOutputInterfaceObjectUsingComRPCConnection();

    // HAL reports the given Atmos capability for the connected sink (STB path: HDMI0).
    void SetAtmosCapability(dsATMOSCapability_t cap)
    {
        ON_CALL(*p_audioOutputPortMock, isConnected()).WillByDefault(Return(true));
        ON_CALL(*p_audioOutputPortMock, getSinkDeviceAtmosCapability(_))
            .WillByDefault(DoAll(::testing::SetArgReferee<0>(cap), Return(true)));
    }

    // HAL exposes one enabled+connected port of the given type and stereo mode.
    void SetSoundMode(int portType, dsAudioStereoMode_t smode, bool stereoAuto = false,
                      const std::string& name = "HDMI0")
    {
        _portName = name;
        ON_CALL(*p_hostImplMock, getAudioOutputPorts())
            .WillByDefault(Return(device::List<device::AudioOutputPort>{device::AudioOutputPort()}));
        ON_CALL(*p_audioOutputPortMock, getType()).WillByDefault(ReturnRef(portTypeObj));
        ON_CALL(*p_audioOutputPortTypeMock, getId()).WillByDefault(Return(portType));
        ON_CALL(*p_audioOutputPortMock, getName()).WillByDefault(ReturnRef(_portName));
        ON_CALL(*p_audioOutputPortMock, isEnabled()).WillByDefault(Return(true));
        ON_CALL(*p_audioOutputPortMock, isConnected()).WillByDefault(Return(true));
        ON_CALL(*p_audioOutputPortMock, getStereoMode())
            .WillByDefault(Return(device::AudioStereoMode(smode)));
        ON_CALL(*p_audioOutputPortMock, getStereoAuto()).WillByDefault(Return(stereoAuto));
    }

    bool Query(Core::hresult expected = Core::ERROR_NONE)
    {
        bool value = false;
        EXPECT_NE(mAudioOutputPlugin, nullptr);
        if (mAudioOutputPlugin != nullptr) {
            EXPECT_EQ(expected, mAudioOutputPlugin->DolbyAtmosExperience(value));
        }
        return value;
    }

    // Lets any notification dispatched by a previous event reach its observers.
    static void Settle() { usleep(300000); }

    void FireAudioMode(int portType, dsAudioStereoMode_t smode)
    {
        ASSERT_NE(dsListener, nullptr);
        dsListener->OnAudioModeEvent(static_cast<dsAudioPortType_t>(portType), smode);
    }

    void FireAtmosCapability(dsATMOSCapability_t cap)
    {
        ASSERT_NE(dsListener, nullptr);
        dsListener->OnDolbyAtmosCapabilitiesChanged(cap, true);
    }

    void FireHdmiHotPlug(bool connected)
    {
        ASSERT_NE(hdmiListener, nullptr);
        hdmiListener->OnHdmiInEventHotPlug(dsHDMI_IN_PORT_0, connected);
    }

private:
    std::string _portName;
};

AudioOutputL2Test::AudioOutputL2Test()
    : L2TestMocks()
{
    TEST_LOG("AudioOutputL2Test constructor");

    ON_CALL(*p_managerImplMock, Initialize()).WillByDefault(Return());
    ON_CALL(*p_managerImplMock, DeInitialize()).WillByDefault(Return());

    // Default: no ports → SoundMode() finds nothing.
    ON_CALL(*p_hostImplMock, getAudioOutputPorts())
        .WillByDefault(Return(device::List<device::AudioOutputPort>{}));

    // AtmosMetadata() always calls getAudioOutputPort(); reference-return has no default action.
    ON_CALL(*p_hostImplMock, getAudioOutputPort(_))
        .WillByDefault(ReturnRef(_audioPortObj));

    // isConnected()=false → AtmosMetadata uses Host::getSinkDeviceAtmosCapability (NOTSUPPORTED).
    ON_CALL(*p_audioOutputPortMock, isConnected())
        .WillByDefault(Return(false));

    // Capture the listeners the plugin registers while it is activated below.
    ON_CALL(*p_hostImplMock, Register(A<device::Host::IAudioOutputPortEvents*>()))
        .WillByDefault(DoAll(SaveArg<0>(&dsListener), Return(dsERR_NONE)));
    ON_CALL(*p_hostImplMock, Register(A<device::Host::IHdmiInEvents*>()))
        .WillByDefault(DoAll(SaveArg<0>(&hdmiListener), Return(dsERR_NONE)));

    uint32_t status = ActivateService(AUDIOOUTPUT_CALLSIGN);
    if (status != Core::ERROR_NONE) {
        TEST_LOG("Failed to activate AudioOutput plugin: %u", status);
    }

    EXPECT_NE(dsListener, nullptr) << "Plugin did not register for IAudioOutputPortEvents";
    EXPECT_NE(hdmiListener, nullptr) << "Plugin did not register for IHdmiInEvents";
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

// Fixture whose HAL reports ATMOSMETADATA capability from the start.
class AudioOutputL2Test_AtmosCapable : public AudioOutputL2Test {
protected:
    AudioOutputL2Test_AtmosCapable() : AudioOutputL2Test()
    {
        SetAtmosCapability(dsAUDIO_ATMOS_ATMOSMETADATA);
    }

    virtual ~AudioOutputL2Test_AtmosCapable() override = default;
};

// ===========================================================================
//  Evaluation
// ===========================================================================

TEST_F(AudioOutputL2Test, JsonRpc_DolbyAtmosExperience_DefaultFalse_WhenHalNotSupported)
{
    JsonObject params, result;
    EXPECT_EQ(Core::ERROR_NONE,
              InvokeServiceMethod(AUDIOOUTPUT_CALLSIGN, "dolbyAtmosExperience", params, result));

    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());
    EXPECT_FALSE(Query()) << "HAL=NOT_SUPPORTED, no ports: must be false";
}

TEST_F(AudioOutputL2Test_AtmosCapable, ComRpc_AtmosCapableAndSurroundMode_ReturnsTrue)
{
    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());
    SetSoundMode(device::AudioOutputPortType::kHDMI, dsAUDIO_STEREO_SURROUND);
    EXPECT_TRUE(Query());
}

TEST_F(AudioOutputL2Test_AtmosCapable, ComRpc_AtmosCapableAndDolbyDigitalPlusMode_ReturnsTrue)
{
    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());
    SetSoundMode(device::AudioOutputPortType::kHDMI, dsAUDIO_STEREO_DDPLUS);
    EXPECT_TRUE(Query());
}

TEST_F(AudioOutputL2Test_AtmosCapable, ComRpc_AtmosCapableAndPassthruMode_ReturnsTrue)
{
    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());
    SetSoundMode(device::AudioOutputPortType::kHDMI, dsAUDIO_STEREO_PASSTHRU);
    EXPECT_TRUE(Query());
}

TEST_F(AudioOutputL2Test_AtmosCapable, ComRpc_ArcStereoAuto_ReturnsTrue)
{
    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());
    // kARC + stereoAuto → SOUNDMODE_AUTO regardless of the reported stereo mode
    SetSoundMode(device::AudioOutputPortType::kARC, dsAUDIO_STEREO_STEREO, true, "HDMI_ARC0");
    EXPECT_TRUE(Query());
}

TEST_F(AudioOutputL2Test_AtmosCapable, ComRpc_NonEnablingModes_ReturnFalse)
{
    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());

    const dsAudioStereoMode_t modes[] = {
        dsAUDIO_STEREO_MONO, dsAUDIO_STEREO_STEREO, dsAUDIO_STEREO_DD, dsAUDIO_STEREO_MAX,
    };
    for (auto mode : modes) {
        SetSoundMode(device::AudioOutputPortType::kHDMI, mode);
        EXPECT_FALSE(Query()) << "mode=" << static_cast<int>(mode);
    }
}

TEST_F(AudioOutputL2Test, ComRpc_NotCapable_ReturnsFalse_EvenWithAtmosStream)
{
    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());
    SetSoundMode(device::AudioOutputPortType::kHDMI, dsAUDIO_STEREO_DDPLUS);
    EXPECT_FALSE(Query());
}

TEST_F(AudioOutputL2Test, ComRpc_SoundModePortTypes)
{
    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());
    SetAtmosCapability(dsAUDIO_ATMOS_ATMOSMETADATA);

    SetSoundMode(device::AudioOutputPortType::kARC, dsAUDIO_STEREO_PASSTHRU, false, "HDMI_ARC0");
    EXPECT_TRUE(Query()) << "ARC passthru";
    SetSoundMode(device::AudioOutputPortType::kSPEAKER, dsAUDIO_STEREO_SURROUND, false, "SPEAKER0");
    EXPECT_TRUE(Query()) << "SPEAKER surround";
    SetSoundMode(device::AudioOutputPortType::kSPDIF, dsAUDIO_STEREO_STEREO, false, "SPDIF0");
    EXPECT_FALSE(Query()) << "SPDIF stereo";
    SetSoundMode(device::AudioOutputPortType::kHEADPHONE, dsAUDIO_STEREO_MONO, false, "HEADPHONE0");
    EXPECT_FALSE(Query()) << "HEADPHONE mono";
}

// ===========================================================================
//  Notifications — event-driven; the getter never notifies
// ===========================================================================

TEST_F(AudioOutputL2Test_AtmosCapable, ComRpc_Notification_FiredWithTrue_OnFalseToTrueTransition)
{
    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());

    auto sink = Core::ProxyType<AudioOutputNotificationSink>::Create();
    mAudioOutputPlugin->Register(&(*sink));

    SetSoundMode(device::AudioOutputPortType::kHDMI, dsAUDIO_STEREO_PASSTHRU);
    FireAudioMode(device::AudioOutputPortType::kHDMI, dsAUDIO_STEREO_PASSTHRU);

    EXPECT_TRUE(sink->WaitForNotification(2000)) << "Notification must fire on false->true";
    EXPECT_TRUE(sink->LastValue());

    mAudioOutputPlugin->Unregister(&(*sink));
}

TEST_F(AudioOutputL2Test_AtmosCapable, ComRpc_Notification_FiredWithFalse_OnTrueToFalseTransition)
{
    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());

    SetSoundMode(device::AudioOutputPortType::kHDMI, dsAUDIO_STEREO_PASSTHRU);
    FireAudioMode(device::AudioOutputPortType::kHDMI, dsAUDIO_STEREO_PASSTHRU);
    Settle();

    auto sink = Core::ProxyType<AudioOutputNotificationSink>::Create();
    mAudioOutputPlugin->Register(&(*sink));

    SetSoundMode(device::AudioOutputPortType::kHDMI, dsAUDIO_STEREO_STEREO);
    FireAudioMode(device::AudioOutputPortType::kHDMI, dsAUDIO_STEREO_STEREO);

    EXPECT_TRUE(sink->WaitForNotification(2000)) << "Notification must fire on true->false";
    EXPECT_FALSE(sink->LastValue());

    mAudioOutputPlugin->Unregister(&(*sink));
}

TEST_F(AudioOutputL2Test_AtmosCapable, ComRpc_Notification_NotFired_WhenValueStaysFalse)
{
    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());

    auto sink = Core::ProxyType<AudioOutputNotificationSink>::Create();
    mAudioOutputPlugin->Register(&(*sink));

    SetSoundMode(device::AudioOutputPortType::kHDMI, dsAUDIO_STEREO_MONO);
    FireAudioMode(device::AudioOutputPortType::kHDMI, dsAUDIO_STEREO_MONO);

    EXPECT_FALSE(sink->WaitForNotification(500));
    mAudioOutputPlugin->Unregister(&(*sink));
}

TEST_F(AudioOutputL2Test_AtmosCapable, ComRpc_Notification_NotFired_WhenValueStaysTrue)
{
    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());

    SetSoundMode(device::AudioOutputPortType::kHDMI, dsAUDIO_STEREO_PASSTHRU);
    FireAudioMode(device::AudioOutputPortType::kHDMI, dsAUDIO_STEREO_PASSTHRU);
    Settle();

    auto sink = Core::ProxyType<AudioOutputNotificationSink>::Create();
    mAudioOutputPlugin->Register(&(*sink));

    SetSoundMode(device::AudioOutputPortType::kHDMI, dsAUDIO_STEREO_DDPLUS);
    FireAudioMode(device::AudioOutputPortType::kHDMI, dsAUDIO_STEREO_DDPLUS);

    EXPECT_FALSE(sink->WaitForNotification(500));
    mAudioOutputPlugin->Unregister(&(*sink));
}

TEST_F(AudioOutputL2Test_AtmosCapable, ComRpc_Notification_NotFired_AfterUnregister)
{
    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());

    auto sink = Core::ProxyType<AudioOutputNotificationSink>::Create();
    mAudioOutputPlugin->Register(&(*sink));
    mAudioOutputPlugin->Unregister(&(*sink));

    SetSoundMode(device::AudioOutputPortType::kHDMI, dsAUDIO_STEREO_PASSTHRU);
    FireAudioMode(device::AudioOutputPortType::kHDMI, dsAUDIO_STEREO_PASSTHRU);

    EXPECT_FALSE(sink->WaitForNotification(500));
}

// Event for ARC while only the speaker is connected must be ignored.
TEST_F(AudioOutputL2Test_AtmosCapable, ComRpc_AudioModeEvent_PortTypeNotConnected_Ignored)
{
    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());

    auto sink = Core::ProxyType<AudioOutputNotificationSink>::Create();
    mAudioOutputPlugin->Register(&(*sink));

    SetSoundMode(device::AudioOutputPortType::kSPEAKER, dsAUDIO_STEREO_PASSTHRU, false, "SPEAKER0");
    FireAudioMode(device::AudioOutputPortType::kARC, dsAUDIO_STEREO_DDPLUS);

    EXPECT_FALSE(sink->WaitForNotification(500));
    mAudioOutputPlugin->Unregister(&(*sink));
}

TEST_F(AudioOutputL2Test_AtmosCapable, ComRpc_AudioModeEvent_GetPortsThrows_Ignored)
{
    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());

    auto sink = Core::ProxyType<AudioOutputNotificationSink>::Create();
    mAudioOutputPlugin->Register(&(*sink));

    ON_CALL(*p_hostImplMock, getAudioOutputPorts())
        .WillByDefault(Throw(device::Exception("Ports unavailable")));
    FireAudioMode(device::AudioOutputPortType::kHDMI, dsAUDIO_STEREO_PASSTHRU);

    EXPECT_FALSE(sink->WaitForNotification(500));
    mAudioOutputPlugin->Unregister(&(*sink));
}

TEST_F(AudioOutputL2Test, ComRpc_Notification_FiredTrue_WhenAtmosCapabilityBecomesSupported)
{
    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());

    SetSoundMode(device::AudioOutputPortType::kHDMI, dsAUDIO_STEREO_PASSTHRU);

    auto sink = Core::ProxyType<AudioOutputNotificationSink>::Create();
    mAudioOutputPlugin->Register(&(*sink));

    FireAtmosCapability(dsAUDIO_ATMOS_ATMOSMETADATA);

    EXPECT_TRUE(sink->WaitForNotification(2000));
    EXPECT_TRUE(sink->LastValue());

    mAudioOutputPlugin->Unregister(&(*sink));
}

TEST_F(AudioOutputL2Test_AtmosCapable, ComRpc_Notification_FiredFalse_WhenAtmosCapabilityBecomesNotSupported)
{
    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());

    SetSoundMode(device::AudioOutputPortType::kHDMI, dsAUDIO_STEREO_PASSTHRU);
    FireAudioMode(device::AudioOutputPortType::kHDMI, dsAUDIO_STEREO_PASSTHRU);
    Settle();

    auto sink = Core::ProxyType<AudioOutputNotificationSink>::Create();
    mAudioOutputPlugin->Register(&(*sink));

    FireAtmosCapability(dsAUDIO_ATMOS_NOTSUPPORTED);

    EXPECT_TRUE(sink->WaitForNotification(2000));
    EXPECT_FALSE(sink->LastValue());

    mAudioOutputPlugin->Unregister(&(*sink));
}

TEST_F(AudioOutputL2Test, ComRpc_Notification_NotFired_WhenCapabilityChangesButResultStaysFalse)
{
    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());

    SetSoundMode(device::AudioOutputPortType::kHDMI, dsAUDIO_STEREO_STEREO);

    auto sink = Core::ProxyType<AudioOutputNotificationSink>::Create();
    mAudioOutputPlugin->Register(&(*sink));

    FireAtmosCapability(dsAUDIO_ATMOS_ATMOSMETADATA);

    EXPECT_FALSE(sink->WaitForNotification(500)) << "STEREO is non-enabling";
    mAudioOutputPlugin->Unregister(&(*sink));
}

// ===========================================================================
//  HDMI-In hot plug re-evaluates the Atmos experience
// ===========================================================================

TEST_F(AudioOutputL2Test_AtmosCapable, ComRpc_HdmiHotPlug_ReevaluatesAndNotifies)
{
    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());

    auto sink = Core::ProxyType<AudioOutputNotificationSink>::Create();
    mAudioOutputPlugin->Register(&(*sink));

    SetSoundMode(device::AudioOutputPortType::kARC, dsAUDIO_STEREO_DDPLUS, false, "HDMI_ARC0");
    FireHdmiHotPlug(true);

    EXPECT_TRUE(sink->WaitForNotification(2000));
    EXPECT_TRUE(sink->LastValue());

    mAudioOutputPlugin->Unregister(&(*sink));
}

TEST_F(AudioOutputL2Test, ComRpc_HdmiHotPlug_ValueUnchanged_NoNotification)
{
    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());

    auto sink = Core::ProxyType<AudioOutputNotificationSink>::Create();
    mAudioOutputPlugin->Register(&(*sink));

    FireHdmiHotPlug(false);

    EXPECT_FALSE(sink->WaitForNotification(500));
    mAudioOutputPlugin->Unregister(&(*sink));
}

TEST_F(AudioOutputL2Test_AtmosCapable, ComRpc_HdmiHotPlug_HalFails_Ignored)
{
    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());

    auto sink = Core::ProxyType<AudioOutputNotificationSink>::Create();
    mAudioOutputPlugin->Register(&(*sink));

    SetSoundMode(device::AudioOutputPortType::kARC, dsAUDIO_STEREO_DDPLUS, false, "HDMI_ARC0");
    ON_CALL(*p_hostImplMock, getAudioOutputPort(::testing::_))
        .WillByDefault(Throw(device::Exception("Simulated HAL failure")));
    FireHdmiHotPlug(true);

    EXPECT_FALSE(sink->WaitForNotification(500));
    mAudioOutputPlugin->Unregister(&(*sink));
}

TEST_F(AudioOutputL2Test_AtmosCapable, ComRpc_Getter_NeverNotifies)
{
    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());

    auto sink = Core::ProxyType<AudioOutputNotificationSink>::Create();
    mAudioOutputPlugin->Register(&(*sink));

    SetSoundMode(device::AudioOutputPortType::kHDMI, dsAUDIO_STEREO_PASSTHRU);
    EXPECT_TRUE(Query());
    EXPECT_TRUE(Query());

    EXPECT_FALSE(sink->WaitForNotification(500));
    mAudioOutputPlugin->Unregister(&(*sink));
}

TEST_F(AudioOutputL2Test, ComRpc_NoNotification_WhenGetterCalledRepeatedlyWithSameState)
{
    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());

    auto sink = Core::ProxyType<AudioOutputNotificationSink>::Create();
    mAudioOutputPlugin->Register(&(*sink));

    EXPECT_FALSE(Query());
    EXPECT_FALSE(Query());
    EXPECT_FALSE(Query());

    EXPECT_FALSE(sink->WaitForNotification(500));
    mAudioOutputPlugin->Unregister(&(*sink));
}

// ===========================================================================
//  Register / Unregister edge cases
// ===========================================================================

TEST_F(AudioOutputL2Test_AtmosCapable, ComRpc_Register_Duplicate_NotificationDeliveredOnce)
{
    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());

    SetSoundMode(device::AudioOutputPortType::kHDMI, dsAUDIO_STEREO_STEREO);

    auto sink = Core::ProxyType<AudioOutputNotificationSink>::Create();
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->Register(&(*sink)));
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->Register(&(*sink)));

    SetSoundMode(device::AudioOutputPortType::kHDMI, dsAUDIO_STEREO_PASSTHRU);
    FireAudioMode(device::AudioOutputPortType::kHDMI, dsAUDIO_STEREO_PASSTHRU);

    EXPECT_TRUE(sink->WaitForNotification(2000));
    mAudioOutputPlugin->Unregister(&(*sink));
}

TEST_F(AudioOutputL2Test, ComRpc_Unregister_NotFound_ReturnsOk)
{
    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());

    auto sink = Core::ProxyType<AudioOutputNotificationSink>::Create();
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->Unregister(&(*sink)));
}

// ===========================================================================
//  HAL failure — the getter reports the error and recovers on the next call
// ===========================================================================

TEST_F(AudioOutputL2Test, DolbyAtmosExperience_HalFailure_ReturnsError_ThenRecovers)
{
    ON_CALL(*p_hostImplMock, getAudioOutputPort(::testing::_))
        .WillByDefault(Throw(device::Exception("Simulated HAL failure")));

    JsonObject params, result;
    EXPECT_NE(Core::ERROR_NONE,
              InvokeServiceMethod(AUDIOOUTPUT_CALLSIGN, "dolbyAtmosExperience", params, result))
        << "HAL failure must surface as an error";

    ON_CALL(*p_hostImplMock, getAudioOutputPort(::testing::_))
        .WillByDefault(ReturnRef(_audioPortObj));

    EXPECT_EQ(Core::ERROR_NONE,
              InvokeServiceMethod(AUDIOOUTPUT_CALLSIGN, "dolbyAtmosExperience", params, result))
        << "Once the HAL recovers the next call must succeed";
}

// ===========================================================================
//  Constructor / destructor exception paths
// ===========================================================================

class AudioOutputL2Test_ManagerInitFailure : public L2TestMocks {
protected:
    device::AudioOutputPort _portObj;

    AudioOutputL2Test_ManagerInitFailure() : L2TestMocks()
    {
        ON_CALL(*p_managerImplMock, Initialize())
            .WillByDefault(Throw(device::Exception("Manager::Initialize failed")));
        ON_CALL(*p_managerImplMock, DeInitialize()).WillByDefault(Return());

        ON_CALL(*p_hostImplMock, getAudioOutputPorts())
            .WillByDefault(Return(device::List<device::AudioOutputPort>{}));
        ON_CALL(*p_hostImplMock, getAudioOutputPort(_))
            .WillByDefault(ReturnRef(_portObj));
        ON_CALL(*p_audioOutputPortMock, isConnected()).WillByDefault(Return(false));

        ActivateService(AUDIOOUTPUT_CALLSIGN);
    }

    virtual ~AudioOutputL2Test_ManagerInitFailure() override
    {
        usleep(CLEANUP_DELAY_MICROSECONDS);
        DeactivateService(AUDIOOUTPUT_CALLSIGN);
    }
};

TEST_F(AudioOutputL2Test_ManagerInitFailure, Constructor_ManagerInitializeFails_PluginStillResponds)
{
    JsonObject params, result;
    EXPECT_EQ(Core::ERROR_NONE,
              InvokeServiceMethod(AUDIOOUTPUT_CALLSIGN, "dolbyAtmosExperience", params, result));
}

class AudioOutputL2Test_ManagerDeInitFailure : public L2TestMocks {
protected:
    device::AudioOutputPort _portObj;

    AudioOutputL2Test_ManagerDeInitFailure() : L2TestMocks()
    {
        ON_CALL(*p_managerImplMock, Initialize()).WillByDefault(Return());
        ON_CALL(*p_managerImplMock, DeInitialize())
            .WillByDefault(Throw(device::Exception("Manager::DeInitialize failed")));

        ON_CALL(*p_hostImplMock, getAudioOutputPorts())
            .WillByDefault(Return(device::List<device::AudioOutputPort>{}));
        ON_CALL(*p_hostImplMock, getAudioOutputPort(_))
            .WillByDefault(ReturnRef(_portObj));
        ON_CALL(*p_audioOutputPortMock, isConnected()).WillByDefault(Return(false));

        EXPECT_EQ(Core::ERROR_NONE, ActivateService(AUDIOOUTPUT_CALLSIGN));
    }

    virtual ~AudioOutputL2Test_ManagerDeInitFailure() override
    {
        usleep(CLEANUP_DELAY_MICROSECONDS);
        // The destructor catch block runs when the plugin is deactivated here.
        DeactivateService(AUDIOOUTPUT_CALLSIGN);
    }
};

TEST_F(AudioOutputL2Test_ManagerDeInitFailure, Destructor_ManagerDeInitializeFails_PluginStillResponds)
{
    JsonObject params, result;
    EXPECT_EQ(Core::ERROR_NONE,
              InvokeServiceMethod(AUDIOOUTPUT_CALLSIGN, "dolbyAtmosExperience", params, result));
}

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

#include "AudioOutputImplementation.h"

// Thunder mocks (ServiceMock, COMLinkMock)
#include "ServiceMock.h"
#include "COMLinkMock.h"

// DeviceSettings mocks — correct filenames from entservices-testframework
#include "HostMock.h"                  // defines HostImplMock  (NOT HostImplMock.h)
#include "AudioOutputPortMock.h"       // defines AudioOutputPortMock
#include "AudioOutputPortTypeMock.h"   // defines AudioOutputPortTypeMock
#include "ManagerMock.h"               // defines ManagerImplMock

#define TEST_LOG(x, ...) \
    fprintf(stderr, "\033[1;32m[%s:%d](%s)<PID:%d><TID:%d>" x "\n\033[0m", \
        __FILE__, __LINE__, __FUNCTION__, getpid(), gettid(), ##__VA_ARGS__); \
    fflush(stderr);

using ::testing::NiceMock;
using ::testing::_;
using ::testing::Return;
using ::testing::ReturnRef;
using ::testing::SaveArg;
using ::testing::DoAll;
using ::testing::SetArgReferee;
using ::testing::Throw;
using ::testing::Eq;
using namespace WPEFramework;

// ---------------------------------------------------------------------------
// Global mock pointers — wired into the devicesettings.h stubs via setImpl().
// Pattern follows entservices-testframework L2TestsMock.
// ---------------------------------------------------------------------------
HostImplMock*            p_hostImplMock            = nullptr;
AudioOutputPortMock*     p_audioOutputPortMock     = nullptr;
AudioOutputPortTypeMock* p_audioOutputPortTypeMock = nullptr;
ManagerImplMock*         p_managerImplMock         = nullptr;

// ---------------------------------------------------------------------------
// Minimal notification mock to capture OnDolbyAtmosExperienceChanged calls
// ---------------------------------------------------------------------------
class MockAudioOutputNotification : public Exchange::IAudioOutput::INotification {
public:
    MOCK_METHOD(void, OnDolbyAtmosExperienceChanged, (const bool dolbyAtmosExperience), (override));

    BEGIN_INTERFACE_MAP(MockAudioOutputNotification)
    INTERFACE_ENTRY(Exchange::IAudioOutput::INotification)
    END_INTERFACE_MAP
};

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------
class AudioOutputImplementationTest : public ::testing::Test {
protected:
    NiceMock<HostImplMock>            hostImplMock;
    NiceMock<AudioOutputPortMock>     audioOutputPortMock;
    NiceMock<AudioOutputPortTypeMock> audioOutputPortTypeMock;
    NiceMock<ManagerImplMock>         managerImplMock;

    // Captured DS event listener — populated during plugin construction
    // when the plugin calls device::Host::getInstance().Register(&listener)
    device::Host::IAudioOutputPortEvents* dsListener = nullptr;

    // A reusable AudioOutputPortType object whose getId() delegates to
    // audioOutputPortTypeMock (via the global AudioOutputPortType::impl).
    device::AudioOutputPortType portTypeObj;

    // A reusable AudioOutputPort object for getAudioOutputPort() return-by-ref.
    // Method calls on it delegate to audioOutputPortMock (via global impl).
    device::AudioOutputPort portObj;

    // Port name strings for getName() ReturnRef — must outlive mock calls.
    std::string portName{"HDMI0"};
    std::string arcPortName{"HDMI_ARC0"};

    Core::ProxyType<Plugin::AudioOutputImplementation> impl;

    void SetUp() override
    {
        // Point global pointers at local NiceMock instances
        p_hostImplMock            = &hostImplMock;
        p_audioOutputPortMock     = &audioOutputPortMock;
        p_audioOutputPortTypeMock = &audioOutputPortTypeMock;
        p_managerImplMock         = &managerImplMock;

        // Wire mocks into the devicesettings stubs so plugin calls reach mocks
        device::Host::setImpl(p_hostImplMock);
        device::AudioOutputPort::setImpl(p_audioOutputPortMock);
        device::AudioOutputPortType::setImpl(p_audioOutputPortTypeMock);
        device::Manager::setImpl(p_managerImplMock);

        // Manager stubs — called by plugin constructor/destructor
        ON_CALL(managerImplMock, Initialize()).WillByDefault(Return());
        ON_CALL(managerImplMock, DeInitialize()).WillByDefault(Return());

        // No audio ports by default → AtmosMetadata / SoundMode / onAudioModeChanged
        // all take the "no port" fast-exit path
        ON_CALL(hostImplMock, getAudioOutputPorts())
            .WillByDefault(Return(device::List<device::AudioOutputPort>{}));

        // Capture the IAudioOutputPortEvents* listener the plugin registers
        // so tests can fire DS HAL event callbacks directly.
        ON_CALL(hostImplMock,
                Register(testing::A<device::Host::IAudioOutputPortEvents*>()))
            .WillByDefault(DoAll(SaveArg<0>(&dsListener),
                                 Return(dsERR_NONE)));
        ON_CALL(hostImplMock,
                UnRegister(testing::A<device::Host::IAudioOutputPortEvents*>()))
            .WillByDefault(Return(dsERR_NONE));

        // Create the implementation under test.
        // The constructor calls Manager::Initialize() then
        // device::Host::getInstance().Register(&_dsAudioPortNotification),
        // which populates dsListener via the SaveArg above.
        impl = Core::ProxyType<Plugin::AudioOutputImplementation>::Create();

        ASSERT_NE(dsListener, nullptr)
            << "Plugin did not call Host::Register in constructor";
    }

    void TearDown() override
    {
        // Release plugin (calls Manager::DeInitialize + Host::UnRegister)
        impl = Core::ProxyType<Plugin::AudioOutputImplementation>();

        // Unwire all stubs — must match setImpl(non-null) above
        device::Host::setImpl(nullptr);
        device::AudioOutputPort::setImpl(nullptr);
        device::AudioOutputPortType::setImpl(nullptr);
        device::Manager::setImpl(nullptr);

        p_hostImplMock            = nullptr;
        p_audioOutputPortMock     = nullptr;
        p_audioOutputPortTypeMock = nullptr;
        p_managerImplMock         = nullptr;
    }

    // ------------------------------------------------------------------
    // Helper: fire OnDolbyAtmosCapabilitiesChanged on the captured listener.
    //
    // This directly drives plugin's onAtmosCapabilitiesChanged() which sets:
    //   _atmosMetaData = (cap == dsAUDIO_ATMOS_ATMOSMETADATA)
    // Then calls UpdateCache() → EvaluateCurrentAtmosExperience() → SendNotify()
    // ------------------------------------------------------------------
    void TriggerAtmosCapabilityChange(dsATMOSCapability_t cap, bool status = true)
    {
        ASSERT_NE(dsListener, nullptr);
        dsListener->OnDolbyAtmosCapabilitiesChanged(cap, status);
    }

    // ------------------------------------------------------------------
    // Helper: fire OnAudioModeEvent on the captured listener.
    //
    // Sets up getAudioOutputPorts() to return ONE port whose type id matches
    // portTypeVal, so onAudioModeChanged() can update _soundMode.
    //
    // Plugin logic in onAudioModeChanged():
    //   - if typeId == portType && (HDMI/ARC/SPDIF/SPEAKER) && getStereoAuto()
    //       → _soundMode = SOUNDMODE_AUTO
    //   - else if typeId == portType
    //       → _soundMode = DsAudioModeToSoundMode(AudioStereoMode(smode))
    // ------------------------------------------------------------------
    void TriggerSoundModeChange(dsAudioPortType_t portTypeVal,
                                dsAudioStereoMode_t smode,
                                bool stereoAuto = false)
    {
        // Return exactly one port whose typeId matches portTypeVal
        ON_CALL(hostImplMock, getAudioOutputPorts())
            .WillByDefault(
                Return(device::List<device::AudioOutputPort>{device::AudioOutputPort()}));

        // audioOutputPortTypeMock.getId() → portTypeVal
        ON_CALL(audioOutputPortTypeMock, getId())
            .WillByDefault(Return(static_cast<int>(portTypeVal)));

        // audioOutputPortMock.getType() → portTypeObj
        // (portTypeObj.getId() delegates to audioOutputPortTypeMock via global impl)
        ON_CALL(audioOutputPortMock, getType())
            .WillByDefault(ReturnRef(portTypeObj));

        // stereoAuto=true → SOUNDMODE_AUTO path; false → DsAudioModeToSoundMode path
        ON_CALL(audioOutputPortMock, getStereoAuto())
            .WillByDefault(Return(stereoAuto));

        ASSERT_NE(dsListener, nullptr);
        dsListener->OnAudioModeEvent(portTypeVal, smode);
    }
};

// ===========================================================================
// Tests: DolbyAtmosExperience — default state
// ===========================================================================

// After construction only (no Configure, no callbacks), state must be false.
TEST_F(AudioOutputImplementationTest, DolbyAtmosExperience_DefaultIsFalse)
{
    bool enabled = true; // intentionally wrong starting value
    EXPECT_EQ(Core::ERROR_NONE, impl->DolbyAtmosExperience(enabled));
    EXPECT_FALSE(enabled) << "Default dolbyAtmosExperience must be false";
}

// ===========================================================================
// Tests: _atmosMetaData = false → always false regardless of sound mode
//
// Even with Atmos-capable sound modes (PASSTHRU, DDPLUS), if the device
// is not Atmos capable (_atmosMetaData=false), EvaluateCurrentAtmosExperience
// returns false at the first guard: "if (!_atmosMetaData) return false;"
// ===========================================================================
TEST_F(AudioOutputImplementationTest, NotSupportedCapability_AllSoundModes_ReturnFalse)
{
    TEST_LOG("_atmosMetaData=false → DolbyAtmosExperience always false");

    // _atmosMetaData stays false (no TriggerAtmosCapabilityChange called)

    const std::vector<std::pair<dsAudioPortType_t, dsAudioStereoMode_t>> modes = {
        {dsAUDIOPORT_TYPE_HDMI, dsAUDIO_STEREO_MONO},
        {dsAUDIOPORT_TYPE_HDMI, dsAUDIO_STEREO_STEREO},
        {dsAUDIOPORT_TYPE_HDMI, dsAUDIO_STEREO_SURROUND},
        {dsAUDIOPORT_TYPE_HDMI, dsAUDIO_STEREO_PASSTHRU},
        {dsAUDIOPORT_TYPE_HDMI, dsAUDIO_STEREO_DDPLUS},
    };

    for (auto& mode : modes) {
        TriggerSoundModeChange(mode.first, mode.second);

        bool enabled = true;
        EXPECT_EQ(Core::ERROR_NONE, impl->DolbyAtmosExperience(enabled));
        EXPECT_FALSE(enabled)
            << "_atmosMetaData=false → false regardless of sound mode="
            << static_cast<int>(mode.second);
    }
}

// ===========================================================================
// Tests: _atmosMetaData = true + enabling sound modes → true
//
// EvaluateCurrentAtmosExperience() returns true only when:
//   _atmosMetaData == true  AND
//   _soundMode in {PASSTHRU, DOLBYDIGITALPLUS, SOUNDMODE_AUTO}
//
// State is driven exclusively through DS HAL event callbacks, which is
// the real production path (no public AudioModeChanged() method exists).
// ===========================================================================

TEST_F(AudioOutputImplementationTest, AtmosMetadata_Passthru_ReturnsTrue)
{
    TEST_LOG("ATMOS_METADATA + PASSTHRU → true");

    // Step 1: DS HAL fires atmos capability event → _atmosMetaData = true
    TriggerAtmosCapabilityChange(dsAUDIO_ATMOS_ATMOSMETADATA);

    // Step 2: DS HAL fires audio mode event → _soundMode = PASSTHRU
    // (dsAUDIO_STEREO_PASSTHRU maps to Exchange::IAudioOutput::PASSTHRU via
    //  DsAudioModeToSoundMode in AudioOutputImplementation.cpp)
    TriggerSoundModeChange(dsAUDIOPORT_TYPE_HDMI, dsAUDIO_STEREO_PASSTHRU);

    bool enabled = false;
    EXPECT_EQ(Core::ERROR_NONE, impl->DolbyAtmosExperience(enabled));
    EXPECT_TRUE(enabled) << "ATMOS_METADATA + PASSTHRU must return true";
}

TEST_F(AudioOutputImplementationTest, AtmosMetadata_DolbyDigitalPlus_ReturnsTrue)
{
    TEST_LOG("ATMOS_METADATA + DOLBYDIGITALPLUS → true");

    TriggerAtmosCapabilityChange(dsAUDIO_ATMOS_ATMOSMETADATA);
    TriggerSoundModeChange(dsAUDIOPORT_TYPE_HDMI, dsAUDIO_STEREO_DDPLUS);

    bool enabled = false;
    EXPECT_EQ(Core::ERROR_NONE, impl->DolbyAtmosExperience(enabled));
    EXPECT_TRUE(enabled) << "ATMOS_METADATA + DOLBYDIGITALPLUS must return true";
}

TEST_F(AudioOutputImplementationTest, AtmosMetadata_SoundModeAuto_ReturnsTrue)
{
    TEST_LOG("ATMOS_METADATA + SOUNDMODE_AUTO → true");

    TriggerAtmosCapabilityChange(dsAUDIO_ATMOS_ATMOSMETADATA);

    // stereoAuto=true → onAudioModeChanged sets _soundMode = SOUNDMODE_AUTO
    // (port type HDMI is in the {HDMI_ARC, SPDIF, HDMI, SPEAKER} list that
    //  qualifies for the SOUNDMODE_AUTO branch when getStereoAuto() is true)
    TriggerSoundModeChange(dsAUDIOPORT_TYPE_HDMI, dsAUDIO_STEREO_PASSTHRU,
                           /*stereoAuto=*/true);

    bool enabled = false;
    EXPECT_EQ(Core::ERROR_NONE, impl->DolbyAtmosExperience(enabled));
    EXPECT_TRUE(enabled) << "ATMOS_METADATA + SOUNDMODE_AUTO must return true";
}

// ===========================================================================
// Tests: _atmosMetaData = true + NON-enabling sound modes → false
//
// Device is Atmos capable but content being decoded is not Atmos
// (MONO/STEREO/SURROUND/DOLBYDIGITAL/UNKNOWN all fall through to default:
//  in EvaluateCurrentAtmosExperience()).
// ===========================================================================
TEST_F(AudioOutputImplementationTest, AtmosMetadata_NonEnablingSoundModes_ReturnFalse)
{
    TEST_LOG("ATMOS_METADATA + non-enabling modes → false");

    TriggerAtmosCapabilityChange(dsAUDIO_ATMOS_ATMOSMETADATA);
    // _atmosMetaData=true from here on, but sound modes below are not enabling

    const std::vector<dsAudioStereoMode_t> nonEnablingModes = {
        dsAUDIO_STEREO_MONO,
        dsAUDIO_STEREO_STEREO,
        dsAUDIO_STEREO_SURROUND,
        dsAUDIO_STEREO_DD,         // legacy Dolby Digital — not Atmos carrier
        dsAUDIO_STEREO_UNKNOWN,
    };

    for (auto smode : nonEnablingModes) {
        TriggerSoundModeChange(dsAUDIOPORT_TYPE_HDMI, smode);

        bool enabled = true;
        EXPECT_EQ(Core::ERROR_NONE, impl->DolbyAtmosExperience(enabled));
        EXPECT_FALSE(enabled)
            << "ATMOS_METADATA + non-enabling mode=" << static_cast<int>(smode)
            << " must return false";
    }
}

// ===========================================================================
// Tests: notifications
// ===========================================================================

// false → true transition fires notification with value=true
TEST_F(AudioOutputImplementationTest, Notification_FiredOnFalseToTrueTransition)
{
    TEST_LOG("Notification fires on false→true transition");

    auto mockNotification = Core::ProxyType<MockAudioOutputNotification>::Create();
    impl->Register(&(*mockNotification));

    // Expect notification with true exactly once (on the transition)
    EXPECT_CALL(*mockNotification, OnDolbyAtmosExperienceChanged(true)).Times(1);

    // Drive: _atmosMetaData=false → true (exp still false, soundMode=UNKNOWN)
    TriggerAtmosCapabilityChange(dsAUDIO_ATMOS_ATMOSMETADATA);

    // Drive: _soundMode=UNKNOWN → PASSTHRU (exp: false→true → notification fires)
    TriggerSoundModeChange(dsAUDIOPORT_TYPE_HDMI, dsAUDIO_STEREO_PASSTHRU);

    impl->Unregister(&(*mockNotification));
}

// true → false transition fires notification with value=false
TEST_F(AudioOutputImplementationTest, Notification_FiredOnTrueToFalseTransition)
{
    TEST_LOG("Notification fires on true→false transition");

    // Drive to true state BEFORE registering the observer
    TriggerAtmosCapabilityChange(dsAUDIO_ATMOS_ATMOSMETADATA);
    TriggerSoundModeChange(dsAUDIOPORT_TYPE_HDMI, dsAUDIO_STEREO_PASSTHRU);
    // _dolbyAtmosExperience = true at this point (no observer, no notification yet)

    auto mockNotification = Core::ProxyType<MockAudioOutputNotification>::Create();
    impl->Register(&(*mockNotification));

    // Expect notification with false exactly once (on the true→false transition)
    EXPECT_CALL(*mockNotification, OnDolbyAtmosExperienceChanged(false)).Times(1);

    // Drive: _soundMode = STEREO → exp: true→false → notification fires
    TriggerSoundModeChange(dsAUDIOPORT_TYPE_HDMI, dsAUDIO_STEREO_STEREO);

    impl->Unregister(&(*mockNotification));
}

// No state change → notification must NOT fire
TEST_F(AudioOutputImplementationTest, Notification_NotFired_WhenStateUnchanged_NoAtmosCapability)
{
    TEST_LOG("No notification when _atmosMetaData=false (value stays false)");

    auto mockNotification = Core::ProxyType<MockAudioOutputNotification>::Create();
    impl->Register(&(*mockNotification));

    // _atmosMetaData=false → EvaluateCurrentAtmosExperience always false
    // false→false is no change → no notification
    EXPECT_CALL(*mockNotification, OnDolbyAtmosExperienceChanged(_)).Times(0);

    TriggerSoundModeChange(dsAUDIOPORT_TYPE_HDMI, dsAUDIO_STEREO_MONO);
    TriggerSoundModeChange(dsAUDIOPORT_TYPE_HDMI, dsAUDIO_STEREO_PASSTHRU);

    impl->Unregister(&(*mockNotification));
}

// Same value repeated → notification fires only once (on first transition)
TEST_F(AudioOutputImplementationTest, Notification_NotFiredWhenValueUnchanged)
{
    TEST_LOG("Notification fires only once when state does not change between calls");

    auto mockNotification = Core::ProxyType<MockAudioOutputNotification>::Create();
    impl->Register(&(*mockNotification));

    // First transition false→true fires notification once
    EXPECT_CALL(*mockNotification, OnDolbyAtmosExperienceChanged(true)).Times(1);

    TriggerAtmosCapabilityChange(dsAUDIO_ATMOS_ATMOSMETADATA);
    TriggerSoundModeChange(dsAUDIOPORT_TYPE_HDMI, dsAUDIO_STEREO_PASSTHRU); // fires once

    // Same sound mode again → exp stays true → no second notification
    TriggerSoundModeChange(dsAUDIOPORT_TYPE_HDMI, dsAUDIO_STEREO_PASSTHRU); // no-op

    impl->Unregister(&(*mockNotification));
}

// ===========================================================================
// Tests: Configure() method — never called in basic tests above.
// Exercises AtmosMetadata() and SoundMode() code paths.
// ===========================================================================

// Configure with no ports: AtmosMetadata takes host-level path, SoundMode
// has no enabled+connected ports → both succeed, no init-failure flags set.
TEST_F(AudioOutputImplementationTest, Configure_NoPorts_BothSucceed)
{
    TEST_LOG("Configure: empty port list → host atmos, no sound mode port");

    // Empty port list (fixture default) → AtmosMetadata loop finds no HDMI_ARC
    // → calls getAudioOutputPort("HDMI0"), isConnected=false → host-level atmos
    ON_CALL(hostImplMock, getAudioOutputPort(Eq(std::string("HDMI0"))))
        .WillByDefault(ReturnRef(portObj));
    // isConnected=false (NiceMock bool default)
    // host getSinkDeviceAtmosCapability is void, no-op by default

    NiceMock<ServiceMock> serviceMock;
    EXPECT_EQ(Core::ERROR_NONE, impl->Configure(&serviceMock));

    // No init failures → DolbyAtmosExperience reads default (false)
    bool enabled = true;
    EXPECT_EQ(Core::ERROR_NONE, impl->DolbyAtmosExperience(enabled));
    EXPECT_FALSE(enabled);
}

// Configure: HDMI0 connected → AtmosMetadata uses port-level getSinkDeviceAtmosCapability.
TEST_F(AudioOutputImplementationTest, Configure_AtmosMetadata_ConnectedHdmi0_AtmosCapable)
{
    TEST_LOG("Configure: HDMI0 connected, atmos capable via port-level call");

    // Empty port list (no HDMI_ARC) → audioPort stays "HDMI0"
    ON_CALL(hostImplMock, getAudioOutputPort(_))
        .WillByDefault(ReturnRef(portObj));
    ON_CALL(audioOutputPortMock, isConnected()).WillByDefault(Return(true));
    // Port's getSinkDeviceAtmosCapability sets ATMOSMETADATA via SetArgReferee
    ON_CALL(audioOutputPortMock, getSinkDeviceAtmosCapability(_))
        .WillByDefault(DoAll(SetArgReferee<0>(dsAUDIO_ATMOS_ATMOSMETADATA),
                             Return(true)));

    NiceMock<ServiceMock> serviceMock;
    EXPECT_EQ(Core::ERROR_NONE, impl->Configure(&serviceMock));

    // _atmosMetaData=true; _soundMode=UNKNOWN (isEnabled=false by NiceMock default)
    // DolbyAtmosExperience: ATMOS_METADATA + UNKNOWN → false
    bool enabled = true;
    EXPECT_EQ(Core::ERROR_NONE, impl->DolbyAtmosExperience(enabled));
    EXPECT_FALSE(enabled);
}

// Configure: port list contains HDMI_ARC → AtmosMetadata detects ARC, uses HDMI_ARC0.
TEST_F(AudioOutputImplementationTest, Configure_AtmosMetadata_HdmiArcPortDetected)
{
    TEST_LOG("Configure: HDMI_ARC0 in port list → AtmosMetadata switches to ARC port");

    // One port whose name contains "HDMI_ARC"
    ON_CALL(hostImplMock, getAudioOutputPorts())
        .WillByDefault(Return(
            device::List<device::AudioOutputPort>{device::AudioOutputPort()}));
    ON_CALL(audioOutputPortMock, getName())
        .WillByDefault(ReturnRef(arcPortName));  // "HDMI_ARC0"
    // After loop: audioPort="HDMI_ARC0" → getAudioOutputPort("HDMI_ARC0")
    ON_CALL(hostImplMock, getAudioOutputPort(_))
        .WillByDefault(ReturnRef(portObj));
    ON_CALL(audioOutputPortMock, isConnected()).WillByDefault(Return(true));
    ON_CALL(audioOutputPortMock, getSinkDeviceAtmosCapability(_))
        .WillByDefault(DoAll(SetArgReferee<0>(dsAUDIO_ATMOS_ATMOSMETADATA),
                             Return(true)));
    // SoundMode: isEnabled=false (NiceMock default) → port not classified
    // → no selected port → mode=UNKNOWN

    NiceMock<ServiceMock> serviceMock;
    EXPECT_EQ(Core::ERROR_NONE, impl->Configure(&serviceMock));

    // _atmosMetaData=true; _soundMode=UNKNOWN → DolbyAtmosExperience=false
    bool enabled = true;
    EXPECT_EQ(Core::ERROR_NONE, impl->DolbyAtmosExperience(enabled));
    EXPECT_FALSE(enabled);
}

// Configure: HDMI port enabled+connected, getStereoMode→DDPLUS.
// Verifies SoundMode classifies the port as HDMI and sets DOLBYDIGITALPLUS.
TEST_F(AudioOutputImplementationTest, Configure_SoundMode_HdmiPort_DolbyDigitalPlus)
{
    TEST_LOG("Configure: HDMI port enabled+connected, DDPLUS → _soundMode=DOLBYDIGITALPLUS");

    ON_CALL(hostImplMock, getAudioOutputPorts())
        .WillByDefault(Return(
            device::List<device::AudioOutputPort>{device::AudioOutputPort()}));
    ON_CALL(audioOutputPortMock, isEnabled()).WillByDefault(Return(true));
    ON_CALL(audioOutputPortMock, isConnected()).WillByDefault(Return(true));
    ON_CALL(audioOutputPortMock, getType()).WillByDefault(ReturnRef(portTypeObj));
    ON_CALL(audioOutputPortTypeMock, getId())
        .WillByDefault(Return(device::AudioOutputPortType::kHDMI));
    ON_CALL(audioOutputPortMock, getName()).WillByDefault(ReturnRef(portName)); // "HDMI0"
    ON_CALL(hostImplMock, getAudioOutputPort(_))
        .WillByDefault(ReturnRef(portObj));
    ON_CALL(audioOutputPortMock, getStereoMode())
        .WillByDefault(Return(device::AudioStereoMode(dsAUDIO_STEREO_DDPLUS)));
    ON_CALL(audioOutputPortMock, getStereoAuto()).WillByDefault(Return(false));
    // AtmosMetadata: isConnected=true → getSinkDeviceAtmosCapability (not set → NOTSUPPORTED)

    NiceMock<ServiceMock> serviceMock;
    EXPECT_EQ(Core::ERROR_NONE, impl->Configure(&serviceMock));

    // _soundMode=DOLBYDIGITALPLUS; inject ATMOS_METADATA → DolbyAtmosExperience=true
    TriggerAtmosCapabilityChange(dsAUDIO_ATMOS_ATMOSMETADATA);
    bool enabled = false;
    EXPECT_EQ(Core::ERROR_NONE, impl->DolbyAtmosExperience(enabled));
    EXPECT_TRUE(enabled)
        << "DDPLUS sound mode + ATMOS_METADATA → dolbyAtmosExperience must be true";
}

// Configure: HDMI port with getStereoAuto=true → SoundMode sets SOUNDMODE_AUTO.
TEST_F(AudioOutputImplementationTest, Configure_SoundMode_HdmiPort_StereoAuto)
{
    TEST_LOG("Configure: HDMI port stereoAuto=true → _soundMode=SOUNDMODE_AUTO");

    ON_CALL(hostImplMock, getAudioOutputPorts())
        .WillByDefault(Return(
            device::List<device::AudioOutputPort>{device::AudioOutputPort()}));
    ON_CALL(audioOutputPortMock, isEnabled()).WillByDefault(Return(true));
    ON_CALL(audioOutputPortMock, isConnected()).WillByDefault(Return(true));
    ON_CALL(audioOutputPortMock, getType()).WillByDefault(ReturnRef(portTypeObj));
    ON_CALL(audioOutputPortTypeMock, getId())
        .WillByDefault(Return(device::AudioOutputPortType::kHDMI));
    ON_CALL(audioOutputPortMock, getName()).WillByDefault(ReturnRef(portName));
    ON_CALL(hostImplMock, getAudioOutputPort(_))
        .WillByDefault(ReturnRef(portObj));
    ON_CALL(audioOutputPortMock, getStereoMode())
        .WillByDefault(Return(device::AudioStereoMode(dsAUDIO_STEREO_PASSTHRU)));
    ON_CALL(audioOutputPortMock, getStereoAuto()).WillByDefault(Return(true));

    NiceMock<ServiceMock> serviceMock;
    EXPECT_EQ(Core::ERROR_NONE, impl->Configure(&serviceMock));

    // _soundMode=SOUNDMODE_AUTO; inject ATMOS_METADATA → DolbyAtmosExperience=true
    TriggerAtmosCapabilityChange(dsAUDIO_ATMOS_ATMOSMETADATA);
    bool enabled = false;
    EXPECT_EQ(Core::ERROR_NONE, impl->DolbyAtmosExperience(enabled));
    EXPECT_TRUE(enabled)
        << "SOUNDMODE_AUTO + ATMOS_METADATA → dolbyAtmosExperience must be true";
}

// Configure: HDMI_ARC port takes SoundMode precedence over other types.
TEST_F(AudioOutputImplementationTest, Configure_SoundMode_HdmiArcPort_Precedence)
{
    TEST_LOG("Configure: kARC port → hdmiArcPorts has priority in SoundMode");

    ON_CALL(hostImplMock, getAudioOutputPorts())
        .WillByDefault(Return(
            device::List<device::AudioOutputPort>{device::AudioOutputPort()}));
    ON_CALL(audioOutputPortMock, isEnabled()).WillByDefault(Return(true));
    ON_CALL(audioOutputPortMock, isConnected()).WillByDefault(Return(true));
    ON_CALL(audioOutputPortMock, getType()).WillByDefault(ReturnRef(portTypeObj));
    ON_CALL(audioOutputPortTypeMock, getId())
        .WillByDefault(Return(device::AudioOutputPortType::kARC));  // HDMI_ARC type
    ON_CALL(audioOutputPortMock, getName()).WillByDefault(ReturnRef(arcPortName));
    ON_CALL(hostImplMock, getAudioOutputPort(_))
        .WillByDefault(ReturnRef(portObj));
    ON_CALL(audioOutputPortMock, getStereoMode())
        .WillByDefault(Return(device::AudioStereoMode(dsAUDIO_STEREO_DDPLUS)));
    ON_CALL(audioOutputPortMock, getStereoAuto()).WillByDefault(Return(false));

    NiceMock<ServiceMock> serviceMock;
    EXPECT_EQ(Core::ERROR_NONE, impl->Configure(&serviceMock));

    // _soundMode=DOLBYDIGITALPLUS (from ARC port); inject ATMOS_METADATA → true
    TriggerAtmosCapabilityChange(dsAUDIO_ATMOS_ATMOSMETADATA);
    bool enabled = false;
    EXPECT_EQ(Core::ERROR_NONE, impl->DolbyAtmosExperience(enabled));
    EXPECT_TRUE(enabled)
        << "HDMI_ARC DDPLUS + ATMOS_METADATA → dolbyAtmosExperience must be true";
}

// ===========================================================================
// Tests: DolbyAtmosExperience retry path (_atmosMetadataInitFailed flag)
// ===========================================================================

// Configure: AtmosMetadata throws (getAudioOutputPort throws) →
// _atmosMetadataInitFailed=true. DolbyAtmosExperience retry: same exception →
// returns ERROR_GENERAL.
TEST_F(AudioOutputImplementationTest, Configure_AtmosMetadata_Exception_RetryAlsoFails_ReturnsErrorGeneral)
{
    TEST_LOG("Configure AtmosMetadata exception → retry fails → ERROR_GENERAL");

    // Make getAudioOutputPort throw device::Exception → AtmosMetadata returns ERROR_GENERAL
    ON_CALL(hostImplMock, getAudioOutputPort(_))
        .WillByDefault(Throw(device::Exception("getAudioOutputPort failed")));
    // SoundMode: empty port list → loop skips → selectedPort empty → ERROR_NONE
    // So _soundModeInitFailed=false, _atmosMetadataInitFailed=true

    NiceMock<ServiceMock> serviceMock;
    EXPECT_EQ(Core::ERROR_NONE, impl->Configure(&serviceMock));

    // DolbyAtmosExperience: _atmosMetadataInitFailed=true → retry
    // AtmosMetadata retry: getAudioOutputPort still throws → atmosErr=true
    // → return ERROR_GENERAL
    bool enabled = false;
    EXPECT_EQ(Core::ERROR_GENERAL, impl->DolbyAtmosExperience(enabled));
}

// Configure: AtmosMetadata throws → _atmosMetadataInitFailed=true.
// DolbyAtmosExperience retry: mock fixed, AtmosMetadata succeeds → clears flag.
TEST_F(AudioOutputImplementationTest, Configure_AtmosMetadata_Exception_RetrySucceeds_ClearsFlag)
{
    TEST_LOG("Configure AtmosMetadata exception → fix mock → retry succeeds");

    // Step 1: configure with exception
    ON_CALL(hostImplMock, getAudioOutputPort(_))
        .WillByDefault(Throw(device::Exception("getAudioOutputPort failed")));

    NiceMock<ServiceMock> serviceMock;
    EXPECT_EQ(Core::ERROR_NONE, impl->Configure(&serviceMock));

    // Step 2: fix the mock — now getAudioOutputPort succeeds
    // isConnected=false → host getSinkDeviceAtmosCapability (no-op, NOTSUPPORTED)
    ON_CALL(hostImplMock, getAudioOutputPort(_))
        .WillByDefault(ReturnRef(portObj));
    ON_CALL(audioOutputPortMock, isConnected()).WillByDefault(Return(false));

    // Step 3: retry succeeds → clears init flags → returns ERROR_NONE
    // _atmosMetaData=false (NOTSUPPORTED), _soundMode=UNKNOWN → enabled=false
    bool enabled = true;
    EXPECT_EQ(Core::ERROR_NONE, impl->DolbyAtmosExperience(enabled));
    EXPECT_FALSE(enabled)
        << "After successful retry: _atmosMetaData=false → dolbyAtmosExperience=false";
}

// Configure: SoundMode throws (isEnabled throws in loop) →
// _soundModeInitFailed=true. DolbyAtmosExperience retry fails → ERROR_GENERAL.
TEST_F(AudioOutputImplementationTest, Configure_SoundMode_Exception_SetsInitFlag_RetryFails)
{
    TEST_LOG("Configure SoundMode exception → retry fails → ERROR_GENERAL");

    // Return one port so SoundMode's loop executes and hits isEnabled()
    ON_CALL(hostImplMock, getAudioOutputPorts())
        .WillByDefault(Return(
            device::List<device::AudioOutputPort>{device::AudioOutputPort()}));
    // AtmosMetadata: getName="HDMI0" (no HDMI_ARC) → host getSinkDeviceAtmosCapability
    ON_CALL(audioOutputPortMock, getName()).WillByDefault(ReturnRef(portName));
    ON_CALL(hostImplMock, getAudioOutputPort(_))
        .WillByDefault(ReturnRef(portObj));
    // isConnected=false for AtmosMetadata → host getSinkDeviceAtmosCapability
    ON_CALL(audioOutputPortMock, isConnected()).WillByDefault(Return(false));
    // SoundMode loop: isEnabled throws → caught → _soundModeInitFailed=true
    ON_CALL(audioOutputPortMock, isEnabled())
        .WillByDefault(Throw(device::Exception("isEnabled failed")));

    NiceMock<ServiceMock> serviceMock;
    EXPECT_EQ(Core::ERROR_NONE, impl->Configure(&serviceMock));
    // _soundModeInitFailed=true, _atmosMetadataInitFailed=false

    // DolbyAtmosExperience: retry → SoundMode throws again → soundErr=true → ERROR_GENERAL
    bool enabled = false;
    EXPECT_EQ(Core::ERROR_GENERAL, impl->DolbyAtmosExperience(enabled));
}

// ===========================================================================
// Tests: Register / Unregister edge cases
// ===========================================================================

// Register the same notification pointer twice → second call logs error but
// does NOT add a duplicate and returns ERROR_NONE.
TEST_F(AudioOutputImplementationTest, Register_DuplicateNotification_ReturnsNone)
{
    TEST_LOG("Register same notification twice → LOGERR on duplicate, no double-add");

    auto mockNotification = Core::ProxyType<MockAudioOutputNotification>::Create();

    EXPECT_EQ(Core::ERROR_NONE, impl->Register(&(*mockNotification)));
    // Second Register of the same pointer → duplicate path (LOGERR)
    EXPECT_EQ(Core::ERROR_NONE, impl->Register(&(*mockNotification)));

    // Only ONE notification should fire (not two) when state changes
    EXPECT_CALL(*mockNotification, OnDolbyAtmosExperienceChanged(true)).Times(1);

    TriggerAtmosCapabilityChange(dsAUDIO_ATMOS_ATMOSMETADATA);
    TriggerSoundModeChange(dsAUDIOPORT_TYPE_HDMI, dsAUDIO_STEREO_PASSTHRU);

    impl->Unregister(&(*mockNotification));
}

// Unregister a notification pointer that was never registered → LOGERR but
// no crash and returns ERROR_NONE.
TEST_F(AudioOutputImplementationTest, Unregister_NotFound_ReturnsNone)
{
    TEST_LOG("Unregister notification that was never registered → LOGERR, no crash");

    auto mockNotification = Core::ProxyType<MockAudioOutputNotification>::Create();
    // NOT registered — Unregister goes to not-found path (LOGERR)
    EXPECT_EQ(Core::ERROR_NONE, impl->Unregister(&(*mockNotification)));

    // Plugin remains functional after the no-op unregister
    bool enabled = true;
    EXPECT_EQ(Core::ERROR_NONE, impl->DolbyAtmosExperience(enabled));
    EXPECT_FALSE(enabled);
}

// ===========================================================================
// Tests: onAudioModeChanged exception path
// ===========================================================================

// When getAudioOutputPorts() throws inside onAudioModeChanged, the exception
// is caught, mode stays UNKNOWN, UpdateCache() is called, plugin keeps working.
TEST_F(AudioOutputImplementationTest, OnAudioModeChanged_GetPortsThrows_ExceptionCaught)
{
    TEST_LOG("onAudioModeChanged: getAudioOutputPorts throws → exception caught silently");

    // Override: getAudioOutputPorts now throws
    ON_CALL(hostImplMock, getAudioOutputPorts())
        .WillByDefault(Throw(device::Exception("getAudioOutputPorts failed")));

    // Trigger the DS HAL callback — exception caught inside onAudioModeChanged
    ASSERT_NE(dsListener, nullptr);
    dsListener->OnAudioModeEvent(dsAUDIOPORT_TYPE_HDMI, dsAUDIO_STEREO_PASSTHRU);

    // Plugin must still be functional after catching the exception
    bool enabled = true;
    EXPECT_EQ(Core::ERROR_NONE, impl->DolbyAtmosExperience(enabled));
    EXPECT_FALSE(enabled)
        << "Exception in onAudioModeChanged should not affect default false state";
}

// ===========================================================================
// Tests: Constructor exception path (standalone — not using the fixture)
// ===========================================================================

// When Manager::Initialize() throws, the constructor catches the exception
// and skips registerDsEventHandlers(), so _registeredDsEventHandlers stays
// false. The plugin must still be usable and the destructor must not crash.
TEST(AudioOutputConstructorTests, ManagerInitializeThrows_ContinuesGracefully)
{
    NiceMock<HostImplMock>            hostMock;
    NiceMock<AudioOutputPortMock>     portMock;
    NiceMock<AudioOutputPortTypeMock> portTypeMock;
    NiceMock<ManagerImplMock>         managerMock;

    device::Host::setImpl(&hostMock);
    device::AudioOutputPort::setImpl(&portMock);
    device::AudioOutputPortType::setImpl(&portTypeMock);
    device::Manager::setImpl(&managerMock);

    // Manager::Initialize throws → constructor catch block fires → registerDsEventHandlers NOT called
    ON_CALL(managerMock, Initialize())
        .WillByDefault(Throw(device::Exception("Manager init failed")));
    ON_CALL(managerMock, DeInitialize()).WillByDefault(Return());
    ON_CALL(hostMock, getAudioOutputPorts())
        .WillByDefault(Return(device::List<device::AudioOutputPort>{}));
    ON_CALL(hostMock, Register(testing::A<device::Host::IAudioOutputPortEvents*>()))
        .WillByDefault(Return(dsERR_NONE));
    ON_CALL(hostMock, UnRegister(testing::A<device::Host::IAudioOutputPortEvents*>()))
        .WillByDefault(Return(dsERR_NONE));

    device::Host::IAudioOutputPortEvents* listener = nullptr;
    // Register() is NOT called when constructor throws — listener stays null
    ON_CALL(hostMock, Register(testing::A<device::Host::IAudioOutputPortEvents*>()))
        .WillByDefault(DoAll(SaveArg<0>(&listener), Return(dsERR_NONE)));

    {
        auto implLocal = Core::ProxyType<Plugin::AudioOutputImplementation>::Create();

        // Register was not called (Initialize threw) → listener is null
        EXPECT_EQ(listener, nullptr)
            << "registerDsEventHandlers must not be called when Initialize throws";

        // Plugin should still return ERROR_NONE (default state = false)
        bool enabled = true;
        EXPECT_EQ(Core::ERROR_NONE, implLocal->DolbyAtmosExperience(enabled));
        EXPECT_FALSE(enabled)
            << "Default dolbyAtmosExperience must be false even after ctor exception";

    } // implLocal destroyed here — destructor must not crash (_registeredDsEventHandlers=false)

    device::Host::setImpl(nullptr);
    device::AudioOutputPort::setImpl(nullptr);
    device::AudioOutputPortType::setImpl(nullptr);
    device::Manager::setImpl(nullptr);
}

// ===========================================================================
// Tests: Destructor — Manager::DeInitialize() throws (lines 69-71)
//
// The destructor calls unregisterDsEventHandlers() then DeInitialize() inside
// a try block.  If DeInitialize() throws, lines 69-71 catch + LOGWARN must
// fire.  The existing ManagerInitializeThrows test covers the constructor
// catch (lines 56-58) but leaves the destructor catch (69-71) at count 0.
// ===========================================================================
TEST(AudioOutputDestructorTests, ManagerDeInitializeThrows_CatchBlockHit)
{
    TEST_LOG("Destructor: DeInitialize throws → catch block (lines 69-71) hit");

    NiceMock<HostImplMock>            hostMock;
    NiceMock<AudioOutputPortMock>     portMock;
    NiceMock<AudioOutputPortTypeMock> portTypeMock;
    NiceMock<ManagerImplMock>         managerMock;

    device::Host::setImpl(&hostMock);
    device::AudioOutputPort::setImpl(&portMock);
    device::AudioOutputPortType::setImpl(&portTypeMock);
    device::Manager::setImpl(&managerMock);

    ON_CALL(managerMock, Initialize()).WillByDefault(Return());
    // DeInitialize throws → destructor catch block (lines 69-71) fired when
    // implLocal is destroyed at end of the inner scope below.
    ON_CALL(managerMock, DeInitialize())
        .WillByDefault(Throw(device::Exception("Manager DeInit failed")));
    ON_CALL(hostMock, getAudioOutputPorts())
        .WillByDefault(Return(device::List<device::AudioOutputPort>{}));
    ON_CALL(hostMock, Register(testing::A<device::Host::IAudioOutputPortEvents*>()))
        .WillByDefault(Return(dsERR_NONE));
    ON_CALL(hostMock, UnRegister(testing::A<device::Host::IAudioOutputPortEvents*>()))
        .WillByDefault(Return(dsERR_NONE));

    {
        auto implLocal = Core::ProxyType<Plugin::AudioOutputImplementation>::Create();

        // Plugin must still be usable despite DeInitialize being wired to throw
        bool enabled = true;
        EXPECT_EQ(Core::ERROR_NONE, implLocal->DolbyAtmosExperience(enabled));
        EXPECT_FALSE(enabled);

    } // implLocal destroyed: unregisterDsEventHandlers OK → DeInitialize throws
      // → catch(device::Exception) at lines 69-71 is hit

    device::Host::setImpl(nullptr);
    device::AudioOutputPort::setImpl(nullptr);
    device::AudioOutputPortType::setImpl(nullptr);
    device::Manager::setImpl(nullptr);
}

// ===========================================================================
// Tests: SoundMode — port-type branches missing from existing tests
//
// Existing tests cover kARC (Configure_SoundMode_HdmiArcPort_Precedence) and
// kHDMI (Configure_SoundMode_HdmiPort_*). The branches for kSPEAKER, kSPDIF,
// and kHEADPHONE (lines 433-438) and their precedence selections (lines
// 449-454) are never exercised.
//
// Additionally, line 467 (kSPEAKER in the stereoAuto OR condition) is 0 because
// all existing tests use kARC or kHDMI which short-circuit before line 467.
// kSPEAKER and kHEADPHONE force evaluation of line 467.
// ===========================================================================

// kSPEAKER port, stereoAuto=false:
//   SoundMode loop   → lines 433-434 (speakerPorts.push_back)
//   Precedence block → lines 449-450 (selectedPort = speakerPorts.front())
//   stereoAuto OR    → line 467 (kSPEAKER check: kARC=F ∧ kSPDIF=F ∧ kHDMI=F → kSPEAKER evaluated)
//   stereoAuto false → OR(true) && false = false → SURROUND stays, not SOUNDMODE_AUTO
TEST_F(AudioOutputImplementationTest, Configure_SoundMode_SpeakerPort_Surround)
{
    TEST_LOG("SoundMode: kSPEAKER stereoAuto=false → lines 433-434, 449-450, 467");

    ON_CALL(hostImplMock, getAudioOutputPorts())
        .WillByDefault(Return(
            device::List<device::AudioOutputPort>{device::AudioOutputPort()}));
    ON_CALL(audioOutputPortMock, isEnabled()).WillByDefault(Return(true));
    ON_CALL(audioOutputPortMock, isConnected()).WillByDefault(Return(true));
    ON_CALL(audioOutputPortMock, getType()).WillByDefault(ReturnRef(portTypeObj));
    ON_CALL(audioOutputPortTypeMock, getId())
        .WillByDefault(Return(device::AudioOutputPortType::kSPEAKER));
    ON_CALL(audioOutputPortMock, getName()).WillByDefault(ReturnRef(portName));
    ON_CALL(hostImplMock, getAudioOutputPort(_)).WillByDefault(ReturnRef(portObj));
    ON_CALL(audioOutputPortMock, getStereoMode())
        .WillByDefault(Return(device::AudioStereoMode(dsAUDIO_STEREO_SURROUND)));
    ON_CALL(audioOutputPortMock, getStereoAuto()).WillByDefault(Return(false));

    NiceMock<ServiceMock> serviceMock;
    EXPECT_EQ(Core::ERROR_NONE, impl->Configure(&serviceMock));

    // _soundMode=SURROUND (non-enabling) → DolbyAtmosExperience=false
    bool enabled = true;
    EXPECT_EQ(Core::ERROR_NONE, impl->DolbyAtmosExperience(enabled));
    EXPECT_FALSE(enabled) << "SURROUND is not an Atmos-enabling mode";
}

// kSPEAKER port, stereoAuto=true:
//   All lines from the Surround test above PLUS line 469 (SOUNDMODE_AUTO via
//   kSPEAKER path — kSPEAKER=true ∧ stereoAuto=true → SOUNDMODE_AUTO)
TEST_F(AudioOutputImplementationTest, Configure_SoundMode_SpeakerPort_StereoAuto)
{
    TEST_LOG("SoundMode: kSPEAKER stereoAuto=true → SOUNDMODE_AUTO via line 467+469");

    ON_CALL(hostImplMock, getAudioOutputPorts())
        .WillByDefault(Return(
            device::List<device::AudioOutputPort>{device::AudioOutputPort()}));
    ON_CALL(audioOutputPortMock, isEnabled()).WillByDefault(Return(true));
    ON_CALL(audioOutputPortMock, isConnected()).WillByDefault(Return(true));
    ON_CALL(audioOutputPortMock, getType()).WillByDefault(ReturnRef(portTypeObj));
    ON_CALL(audioOutputPortTypeMock, getId())
        .WillByDefault(Return(device::AudioOutputPortType::kSPEAKER));
    ON_CALL(audioOutputPortMock, getName()).WillByDefault(ReturnRef(portName));
    ON_CALL(hostImplMock, getAudioOutputPort(_)).WillByDefault(ReturnRef(portObj));
    ON_CALL(audioOutputPortMock, getStereoMode())
        .WillByDefault(Return(device::AudioStereoMode(dsAUDIO_STEREO_PASSTHRU)));
    ON_CALL(audioOutputPortMock, getStereoAuto()).WillByDefault(Return(true));

    NiceMock<ServiceMock> serviceMock;
    EXPECT_EQ(Core::ERROR_NONE, impl->Configure(&serviceMock));

    // _soundMode=SOUNDMODE_AUTO (enabling) + inject ATMOS_METADATA → true
    TriggerAtmosCapabilityChange(dsAUDIO_ATMOS_ATMOSMETADATA);
    bool enabled = false;
    EXPECT_EQ(Core::ERROR_NONE, impl->DolbyAtmosExperience(enabled));
    EXPECT_TRUE(enabled) << "SPEAKER stereoAuto=true → SOUNDMODE_AUTO + ATMOS_METADATA = true";
}

// kSPDIF port:
//   SoundMode loop   → lines 435-436 (spdifPorts.push_back)
//   Precedence block → lines 451-452 (selectedPort = spdifPorts.front())
//   stereoAuto OR    → kSPDIF=true short-circuits at line 465 (kHDMI/kSPEAKER not reached)
TEST_F(AudioOutputImplementationTest, Configure_SoundMode_SpdifPort_Passthru)
{
    TEST_LOG("SoundMode: kSPDIF port → lines 435-436, 451-452");

    ON_CALL(hostImplMock, getAudioOutputPorts())
        .WillByDefault(Return(
            device::List<device::AudioOutputPort>{device::AudioOutputPort()}));
    ON_CALL(audioOutputPortMock, isEnabled()).WillByDefault(Return(true));
    ON_CALL(audioOutputPortMock, isConnected()).WillByDefault(Return(true));
    ON_CALL(audioOutputPortMock, getType()).WillByDefault(ReturnRef(portTypeObj));
    ON_CALL(audioOutputPortTypeMock, getId())
        .WillByDefault(Return(device::AudioOutputPortType::kSPDIF));
    ON_CALL(audioOutputPortMock, getName()).WillByDefault(ReturnRef(portName));
    ON_CALL(hostImplMock, getAudioOutputPort(_)).WillByDefault(ReturnRef(portObj));
    ON_CALL(audioOutputPortMock, getStereoMode())
        .WillByDefault(Return(device::AudioStereoMode(dsAUDIO_STEREO_PASSTHRU)));
    ON_CALL(audioOutputPortMock, getStereoAuto()).WillByDefault(Return(false));

    NiceMock<ServiceMock> serviceMock;
    EXPECT_EQ(Core::ERROR_NONE, impl->Configure(&serviceMock));

    // _soundMode=PASSTHRU (enabling) + inject ATMOS_METADATA → true
    TriggerAtmosCapabilityChange(dsAUDIO_ATMOS_ATMOSMETADATA);
    bool enabled = false;
    EXPECT_EQ(Core::ERROR_NONE, impl->DolbyAtmosExperience(enabled));
    EXPECT_TRUE(enabled) << "SPDIF PASSTHRU + ATMOS_METADATA = true";
}

// kHEADPHONE port:
//   SoundMode loop   → lines 437-438 (headphonePorts.push_back)
//   Precedence block → lines 453-454 (selectedPort = headphonePorts.front())
//   stereoAuto OR    → kHEADPHONE is NOT in the condition, so kARC=F, kSPDIF=F,
//                      kHDMI=F, kSPEAKER=F → ALL four sub-conditions evaluated
//                      → line 467 (kSPEAKER) hit with result=false
//                      → getStereoAuto() NOT called (OR short-circuits as false)
TEST_F(AudioOutputImplementationTest, Configure_SoundMode_HeadphonePort_Mono)
{
    TEST_LOG("SoundMode: kHEADPHONE port → lines 437-438, 453-454, 467(false path)");

    ON_CALL(hostImplMock, getAudioOutputPorts())
        .WillByDefault(Return(
            device::List<device::AudioOutputPort>{device::AudioOutputPort()}));
    ON_CALL(audioOutputPortMock, isEnabled()).WillByDefault(Return(true));
    ON_CALL(audioOutputPortMock, isConnected()).WillByDefault(Return(true));
    ON_CALL(audioOutputPortMock, getType()).WillByDefault(ReturnRef(portTypeObj));
    ON_CALL(audioOutputPortTypeMock, getId())
        .WillByDefault(Return(device::AudioOutputPortType::kHEADPHONE));
    ON_CALL(audioOutputPortMock, getName()).WillByDefault(ReturnRef(portName));
    ON_CALL(hostImplMock, getAudioOutputPort(_)).WillByDefault(ReturnRef(portObj));
    ON_CALL(audioOutputPortMock, getStereoMode())
        .WillByDefault(Return(device::AudioStereoMode(dsAUDIO_STEREO_MONO)));
    ON_CALL(audioOutputPortMock, getStereoAuto()).WillByDefault(Return(false));

    NiceMock<ServiceMock> serviceMock;
    EXPECT_EQ(Core::ERROR_NONE, impl->Configure(&serviceMock));

    // _soundMode=MONO (non-enabling) → DolbyAtmosExperience=false
    bool enabled = true;
    EXPECT_EQ(Core::ERROR_NONE, impl->DolbyAtmosExperience(enabled));
    EXPECT_FALSE(enabled) << "MONO is not an Atmos-enabling mode";
}

// Selected port is no longer connected between the loop check and the inner
// isConnected() check → line 474 (LOGWARN "no longer connected") hit.
//
// isConnected() call sequence across Configure():
//   call 1 — AtmosMetadata STB: getAudioOutputPort("HDMI0").isConnected()  → false
//   call 2 — SoundMode loop:    aPort.isConnected()                        → true  (port classified)
//   call 3 — SoundMode inner:   getAudioOutputPort(selected).isConnected() → false (→ line 474)
TEST_F(AudioOutputImplementationTest, Configure_SoundMode_SelectedPortNoLongerConnected)
{
    TEST_LOG("SoundMode: port disconnected in inner check → line 474 hit");

    ON_CALL(hostImplMock, getAudioOutputPorts())
        .WillByDefault(Return(
            device::List<device::AudioOutputPort>{device::AudioOutputPort()}));
    ON_CALL(audioOutputPortMock, isEnabled()).WillByDefault(Return(true));
    // Sequence: false (AtmosMetadata), true (SoundMode loop), false (SoundMode inner)
    EXPECT_CALL(audioOutputPortMock, isConnected())
        .WillOnce(Return(false))        // AtmosMetadata: HDMI0 not connected → host-level path
        .WillOnce(Return(true))         // SoundMode loop: enabled && connected → classify to hdmiPorts
        .WillRepeatedly(Return(false)); // SoundMode inner: getAudioOutputPort(selected).isConnected()
                                        //                  → false → LOGWARN (line 474)
    ON_CALL(audioOutputPortMock, getType()).WillByDefault(ReturnRef(portTypeObj));
    ON_CALL(audioOutputPortTypeMock, getId())
        .WillByDefault(Return(device::AudioOutputPortType::kHDMI));
    ON_CALL(audioOutputPortMock, getName()).WillByDefault(ReturnRef(portName));
    ON_CALL(hostImplMock, getAudioOutputPort(_)).WillByDefault(ReturnRef(portObj));

    NiceMock<ServiceMock> serviceMock;
    EXPECT_EQ(Core::ERROR_NONE, impl->Configure(&serviceMock));

    // _soundMode=UNKNOWN (inner check failed) → DolbyAtmosExperience=false
    bool enabled = true;
    EXPECT_EQ(Core::ERROR_NONE, impl->DolbyAtmosExperience(enabled));
    EXPECT_FALSE(enabled) << "UNKNOWN sound mode → dolbyAtmosExperience must be false";
}

// ===========================================================================
// Tests: DsAudioPortNotification stub overrides (lines 83-89)
//
// OnAudioOutHotPlug, OnAudioFormatUpdate, OnAudioPortStateChanged,
// OnAssociatedAudioMixingChanged, OnAudioFaderControlChanged,
// OnAudioPrimaryLanguageChanged, OnAudioSecondaryLanguageChanged
// are all no-op stubs. Fire each callback through dsListener to hit them.
// ===========================================================================
TEST_F(AudioOutputImplementationTest, DsAudioPortNotification_StubOverrides_NoCrash)
{
    TEST_LOG("DsAudioPortNotification stub overrides: fire all no-op callbacks");

    ASSERT_NE(dsListener, nullptr);

    // Each call hits one stub line and returns immediately (no side effects).
    dsListener->OnAudioOutHotPlug(dsAUDIOPORT_TYPE_HDMI, 0, true);         // line 83
    dsListener->OnAudioFormatUpdate(dsAUDIO_FORMAT_PCM);                    // line 84
    dsListener->OnAudioPortStateChanged(dsAUDIOPORT_STATE_INITIALIZED);     // line 85
    dsListener->OnAssociatedAudioMixingChanged(true);                       // line 86
    dsListener->OnAudioFaderControlChanged(50);                             // line 87
    dsListener->OnAudioPrimaryLanguageChanged("eng");                       // line 88
    dsListener->OnAudioSecondaryLanguageChanged("fra");                     // line 89

    // Plugin must remain functional after all stub calls
    bool enabled = true;
    EXPECT_EQ(Core::ERROR_NONE, impl->DolbyAtmosExperience(enabled));
    EXPECT_FALSE(enabled);
}

// ===========================================================================
// Tests: INTERFACE_MAP (lines 50-52)
//
// BEGIN_INTERFACE_MAP / INTERFACE_ENTRY generates a QueryInterface override.
// Calling QueryInterface<IAudioOutput> and QueryInterface<IConfiguration>
// exercises lines 50-52 and the interface dispatch table.
// ===========================================================================
TEST_F(AudioOutputImplementationTest, InterfaceMap_QueryInterface_AudioOutput)
{
    TEST_LOG("INTERFACE_MAP: QueryInterface<IAudioOutput> → non-null");

    auto* iface = impl->QueryInterface<Exchange::IAudioOutput>();
    ASSERT_NE(iface, nullptr)
        << "QueryInterface<IAudioOutput> must return non-null";
    iface->Release();
}

TEST_F(AudioOutputImplementationTest, InterfaceMap_QueryInterface_Configuration)
{
    TEST_LOG("INTERFACE_MAP: QueryInterface<IConfiguration> → non-null");

    auto* iface = impl->QueryInterface<Exchange::IConfiguration>();
    ASSERT_NE(iface, nullptr)
        << "QueryInterface<IConfiguration> must return non-null";
    iface->Release();
}

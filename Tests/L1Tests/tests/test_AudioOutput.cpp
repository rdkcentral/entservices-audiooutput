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

    for (auto& [pt, sm] : modes) {
        TriggerSoundModeChange(pt, sm);

        bool enabled = true;
        EXPECT_EQ(Core::ERROR_NONE, impl->DolbyAtmosExperience(enabled));
        EXPECT_FALSE(enabled)
            << "_atmosMetaData=false → false regardless of sound mode="
            << static_cast<int>(sm);
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

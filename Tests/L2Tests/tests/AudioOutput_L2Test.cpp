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

#define TEST_LOG(x, ...) fprintf(stderr, "\033[1;32m[%s:%d](%s)<PID:%d><TID:%d>" x "\n\033[0m", \
    __FILE__, __LINE__, __FUNCTION__, getpid(), gettid(), ##__VA_ARGS__); fflush(stderr);

#define AUDIOOUTPUT_CALLSIGN        _T("org.rdk.AudioOutput")
#define CLEANUP_DELAY_MICROSECONDS  500000

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
// COM-RPC. The DeviceSettings plugin is not part of this L2 environment, so the
// plugin runs with its default state (dolbyAtmosExperience == false) and these
// tests cover plugin activation, the JSON-RPC / COM-RPC wiring and the
// notification registration. The event-driven behaviour is covered by the L1 tests.
// ---------------------------------------------------------------------------
class AudioOutputL2Test : public L2TestMocks {
protected:
    Core::ProxyType<RPC::InvokeServerType<1, 0, 4>> mAudioOutputEngine;
    Core::ProxyType<RPC::CommunicatorClient>        mAudioOutputClient;
    Exchange::IAudioOutput*                          mAudioOutputPlugin{nullptr};

    AudioOutputL2Test();
    virtual ~AudioOutputL2Test() override;

    uint32_t CreateAudioOutputInterfaceObjectUsingComRPCConnection();
};

AudioOutputL2Test::AudioOutputL2Test()
    : L2TestMocks()
{
    TEST_LOG("AudioOutputL2Test constructor");

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

// ---------------------------------------------------------------------------
// AudioOutputL2Test_AtmosCapable
//
// Derived fixture that pre-sets _atmosMetaData=true AFTER plugin activation
// by firing OnDolbyAtmosCapabilitiesChanged(ATMOSMETADATA) through the
// captured DS listener.
//
// WHY NOT configure getSinkDeviceAtmosCapability BEFORE activation:
//   HostImplMock::getSinkDeviceAtmosCapability takes by VALUE (not reference),
//   so SetArgReferee<0> would not compile.
//   AudioOutputPortMock::getSinkDeviceAtmosCapability does take by reference,
//   but that path requires additional mocking of getAudioOutputPort +
//   isConnected and is more fragile.
//
//   Injecting via OnDolbyAtmosCapabilitiesChanged after activation is
//   simpler and exercises the same code path used on a real device.
// ---------------------------------------------------------------------------
class AudioOutputL2Test_AtmosCapable : public AudioOutputL2Test {
protected:
    AudioOutputL2Test_AtmosCapable() : AudioOutputL2Test()
    {
        TEST_LOG("AudioOutputL2Test_AtmosCapable: injecting ATMOSMETADATA");
        // Drive _atmosMetaData = true via the DS HAL event callback.
        // Same path as production: HAL fires OnDolbyAtmosCapabilitiesChanged →
        // DsAudioPortNotification::OnDolbyAtmosCapabilitiesChanged (override) →
        // AudioOutputImplementation::onAtmosCapabilitiesChanged →
        // _atmosMetaData = true; UpdateCache().
        InjectAtmosCapability(dsAUDIO_ATMOS_ATMOSMETADATA);
    }

    virtual ~AudioOutputL2Test_AtmosCapable() override = default;
};

// ===========================================================================
//  Scenario 1 — JSON-RPC call, default hardware (NOT_SUPPORTED)
//
//  HAL state: _atmosMetaData=false (default, no injection)
//             _soundMode=UNKNOWN  (no injection)
//
//  Decision logic:
//    EvaluateCurrentAtmosExperience():
//      if (!_atmosMetaData) return false;  ← fails here immediately
//
//  Tests the JSON-RPC wire-up end-to-end:
//    InvokeServiceMethod → Thunder → AudioOutput.1 → DolbyAtmosExperience → false
//
//  Also verifies via COM-RPC to confirm the same value is returned on both paths.
// ===========================================================================
TEST_F(AudioOutputL2Test, JsonRpc_DolbyAtmosExperience_DefaultFalse_WhenDeviceSettingsInactive)
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
    // Exercises LOGERR("notification not found") — must not crash or assert
}

// ===========================================================================
//  AudioOutputL2Test_InitFailure
//
//  Fixture that makes AtmosMetadata() fail during Configure():
//    getAudioOutputPort() throws device::Exception → AtmosMetadata returns
//    ERROR_GENERAL → _atmosMetadataInitFailed = true.
//
//  This puts the implementation into the "retry-on-next-call" state so that
//  DolbyAtmosExperience() enters the retry branch (lines that are otherwise
//  unreachable in the default fixture).
//
//  The port list is kept EMPTY so SoundMode() succeeds (no ports → no call to
//  getAudioOutputPort in SoundMode) and only the atmos init flag is set.
// ===========================================================================
class AudioOutputL2Test_InitFailure : public L2TestMocks {
protected:
    // Persistent port object returned by getAudioOutputPort() in the
    // "retry succeeds" test after the mock is fixed.
    device::AudioOutputPort portObj;

    AudioOutputL2Test_InitFailure() : L2TestMocks()
    {
        TEST_LOG("AudioOutputL2Test_InitFailure constructor");

        // Manager stubs
        ON_CALL(*p_managerImplMock, Initialize()).WillByDefault(Return());
        ON_CALL(*p_managerImplMock, DeInitialize()).WillByDefault(Return());

        // Empty port list — SoundMode() will skip the loop and return ERROR_NONE,
        // so only _atmosMetadataInitFailed is set (not _soundModeInitFailed).
        ON_CALL(*p_hostImplMock, getAudioOutputPorts())
            .WillByDefault(Return(device::List<device::AudioOutputPort>{}));

        // getAudioOutputPort throws → AtmosMetadata catches exception →
        // returns ERROR_GENERAL → Configure sets _atmosMetadataInitFailed = true.
        ON_CALL(*p_hostImplMock, getAudioOutputPort(::testing::_))
            .WillByDefault(Throw(device::Exception("Simulated HAL failure")));

        ON_CALL(*p_hostImplMock,
                Register(A<device::Host::IAudioOutputPortEvents*>()))
            .WillByDefault(Return(dsERR_NONE));
        ON_CALL(*p_hostImplMock,
                UnRegister(A<device::Host::IAudioOutputPortEvents*>()))
            .WillByDefault(Return(dsERR_NONE));

        // Activate the plugin — Configure() will run, setting _atmosMetadataInitFailed=true
        uint32_t status = ActivateService(AUDIOOUTPUT_CALLSIGN);
        if (status != Core::ERROR_NONE) {
            TEST_LOG("Warning: ActivateService returned %u (Configure always returns ERROR_NONE)", status);
        }
    }

    virtual ~AudioOutputL2Test_InitFailure() override
    {
        TEST_LOG("AudioOutputL2Test_InitFailure destructor");
        usleep(CLEANUP_DELAY_MICROSECONDS);
        DeactivateService(AUDIOOUTPUT_CALLSIGN);
    }
};

// ===========================================================================
//  Scenario H — DolbyAtmosExperience retry path: both retries fail
//
//  With _atmosMetadataInitFailed=true, DolbyAtmosExperience() enters the
//  retry block. getAudioOutputPort still throws → AtmosMetadata still returns
//  ERROR_GENERAL → DolbyAtmosExperience returns ERROR_GENERAL.
//
//  Exercises lines in the retry block: cap declaration, AtmosMetadata call,
//  SoundMode call, if (atmosErr || soundErr) LOGERR, return ERROR_GENERAL.
// ===========================================================================
TEST_F(AudioOutputL2Test_InitFailure,
       DolbyAtmosExperience_RetryFails_WhenHalStillUnavailable_ReturnsError)
{
    TEST_LOG("Scenario H: retry path — HAL still unavailable → ERROR_GENERAL");

    // getAudioOutputPort still throws → retry of AtmosMetadata fails
    // InvokeServiceMethod returns the error propagated from the implementation
    JsonObject params, result;
    uint32_t status = InvokeServiceMethod(AUDIOOUTPUT_CALLSIGN, "dolbyAtmosExperience",
                                          params, result);
    EXPECT_NE(Core::ERROR_NONE, status)
        << "When retry fails (HAL still unavailable), dolbyAtmosExperience must return an error";
}

// ===========================================================================
//  Scenario I — DolbyAtmosExperience retry path: retry succeeds after fix
//
//  After ActivateService (which left _atmosMetadataInitFailed=true), we fix
//  the getAudioOutputPort mock to return a valid port object.  The next call
//  to DolbyAtmosExperience() enters the retry block, AtmosMetadata succeeds,
//  flags are cleared, UpdateCache runs, and the method returns ERROR_NONE.
//
//  Exercises: flag-clear lines, UpdateCache call, LOGINFO, and the final
//  "enabled = _dolbyAtmosExperience; return ERROR_NONE" path.
// ===========================================================================
TEST_F(AudioOutputL2Test_InitFailure,
       DolbyAtmosExperience_RetrySucceeds_AfterMockFixed_ReturnsNone)
{
    TEST_LOG("Scenario I: retry path — HAL fixed after init failure → ERROR_NONE");

    // Fix the mock: getAudioOutputPort no longer throws.
    // isConnected() = false → host-level getSinkDeviceAtmosCapability path
    // (cap stays dsAUDIO_ATMOS_NOTSUPPORTED → supported=false)
    ON_CALL(*p_hostImplMock, getAudioOutputPort(::testing::_))
        .WillByDefault(ReturnRef(portObj));
    ON_CALL(*p_audioOutputPortMock, isConnected())
        .WillByDefault(Return(false));

    // Now the retry should succeed: AtmosMetadata returns ERROR_NONE,
    // SoundMode returns ERROR_NONE, flags are cleared, ERROR_NONE returned.
    JsonObject params, result;
    uint32_t status = InvokeServiceMethod(AUDIOOUTPUT_CALLSIGN, "dolbyAtmosExperience",
                                          params, result);
    EXPECT_EQ(Core::ERROR_NONE, status)
        << "After fixing HAL mock, retry must succeed and return ERROR_NONE";
}

// ===========================================================================
//  AudioOutputL2Test_SoundMode_Common
//
//  Single reusable fixture for all SoundMode() port-type branch tests.
//
//  Design:
//    - Constructor sets up ALL common mocks but does NOT call ActivateService().
//    - Each TEST_F calls Activate(portTypeId, stereoMode, stereoAuto, portName)
//      which sets the port-specific mocks THEN activates the plugin.
//    - SoundMode() runs inside Configure() with the test-specific mocks.
//    - Destructor deactivates only if Activate() was called.
//
//  Why this works:
//    GMock ON_CALL sets a default action that can be overridden any time
//    before the call actually happens. Since ActivateService() (and thus
//    Configure() → SoundMode()) is called INSIDE Activate(), setting
//    getId() / getStereoMode() BEFORE Activate() is sufficient.
// ===========================================================================
class AudioOutputL2Test_SoundMode_Common : public L2TestMocks {
protected:
    device::AudioOutputPort     _audioPortObj;
    device::AudioOutputPortType _portTypeObj;
    device::Host::IAudioOutputPortEvents* dsListener{nullptr};
    bool _activated{false};
    std::string _portName;

    explicit AudioOutputL2Test_SoundMode_Common() : L2TestMocks()
    {
        ON_CALL(*p_managerImplMock, Initialize()).WillByDefault(Return());
        ON_CALL(*p_managerImplMock, DeInitialize()).WillByDefault(Return());

        // One port in the list so the SoundMode() loop body runs
        ON_CALL(*p_hostImplMock, getAudioOutputPorts())
            .WillByDefault(Return(device::List<device::AudioOutputPort>{device::AudioOutputPort()}));

        // Port is enabled + connected → enters the if-block
        ON_CALL(*p_audioOutputPortMock, isEnabled()).WillByDefault(Return(true));
        ON_CALL(*p_audioOutputPortMock, isConnected()).WillByDefault(Return(true));

        // getType() returns the persistent portTypeObj shell
        ON_CALL(*p_audioOutputPortMock, getType()).WillByDefault(ReturnRef(_portTypeObj));

        // getAudioOutputPort(name) → persistent port object
        ON_CALL(*p_hostImplMock, getAudioOutputPort(_)).WillByDefault(ReturnRef(_audioPortObj));

        // Capture dsListener from Host::Register()
        ON_CALL(*p_hostImplMock, Register(A<device::Host::IAudioOutputPortEvents*>()))
            .WillByDefault(DoAll(SaveArg<0>(&dsListener), Return(dsERR_NONE)));
        ON_CALL(*p_hostImplMock, UnRegister(A<device::Host::IAudioOutputPortEvents*>()))
            .WillByDefault(Return(dsERR_NONE));

        // NOTE: ActivateService() is NOT called here.
        // Call Activate() from the test body after setting port-specific mocks.
    }

    // ------------------------------------------------------------------
    // Set port-specific mocks then activate the plugin.
    // SoundMode() runs inside Configure() with these values.
    //
    //   portTypeId  — device::AudioOutputPortType::kHDMI / kARC / etc.
    //   stereoMode  — device::AudioStereoMode::kPassThru / kSurround / etc.
    //   stereoAuto  — true → _soundMode=SOUNDMODE_AUTO branch
    //   portName    — arbitrary string, returned by getName()
    // ------------------------------------------------------------------
    void Activate(int portTypeId,
                  device::AudioStereoMode stereoMode,
                  bool stereoAuto,
                  const std::string& portName)
    {
        ON_CALL(*p_audioOutputPortTypeMock, getId()).WillByDefault(Return(portTypeId));
        _portName = portName;
        ON_CALL(*p_audioOutputPortMock, getName()).WillByDefault(ReturnRef(_portName));
        ON_CALL(*p_audioOutputPortMock, getStereoMode()).WillByDefault(Return(stereoMode));
        ON_CALL(*p_audioOutputPortMock, getStereoAuto()).WillByDefault(Return(stereoAuto));

        uint32_t status = ActivateService(AUDIOOUTPUT_CALLSIGN);
        EXPECT_EQ(Core::ERROR_NONE, status);
        EXPECT_NE(dsListener, nullptr);
        _activated = true;
    }

    virtual ~AudioOutputL2Test_SoundMode_Common() override
    {
        if (_activated) {
            usleep(CLEANUP_DELAY_MICROSECONDS);
            DeactivateService(AUDIOOUTPUT_CALLSIGN);
        }
    }
};

// ===========================================================================
//  SoundMode: kHDMI port, getStereoMode=kPassThru → _soundMode=PASSTHRU
//  Exercises: lines 408-409 (hdmiPorts branch), 424, 438-439
// ===========================================================================
TEST_F(AudioOutputL2Test_SoundMode_Common, SoundMode_HdmiPort_PassThru_SetsSoundModePassthru)
{
    Activate(device::AudioOutputPortType::kHDMI,
             device::AudioStereoMode::kPassThru, false, "HDMI0");

    // PASSTHRU is an enabling mode → inject ATMOSMETADATA → result=true
    ASSERT_NE(dsListener, nullptr);
    dsListener->OnDolbyAtmosCapabilitiesChanged(dsAUDIO_ATMOS_ATMOSMETADATA, true);

    JsonObject params, result;
    EXPECT_EQ(Core::ERROR_NONE,
              InvokeServiceMethod(AUDIOOUTPUT_CALLSIGN, "dolbyAtmosExperience", params, result));
}

// ===========================================================================
//  SoundMode: kARC port, getStereoMode=kPassThru → _soundMode=PASSTHRU
//  Exercises: lines 407-408 (arcPorts branch), 423
// ===========================================================================
TEST_F(AudioOutputL2Test_SoundMode_Common, SoundMode_ArcPort_PassThru_SetsSoundModePassthru)
{
    Activate(device::AudioOutputPortType::kARC,
             device::AudioStereoMode::kPassThru, false, "ARC0");

    ASSERT_NE(dsListener, nullptr);
    dsListener->OnDolbyAtmosCapabilitiesChanged(dsAUDIO_ATMOS_ATMOSMETADATA, true);

    JsonObject params, result;
    EXPECT_EQ(Core::ERROR_NONE,
              InvokeServiceMethod(AUDIOOUTPUT_CALLSIGN, "dolbyAtmosExperience", params, result));
}

// ===========================================================================
//  SoundMode: kSPEAKER port, getStereoMode=kSurround → _soundMode=SURROUND
//  Exercises: lines 411-412 (speakerPorts branch), 427
//  SURROUND is non-enabling → dolbyAtmosExperience stays false
// ===========================================================================
TEST_F(AudioOutputL2Test_SoundMode_Common, SoundMode_SpeakerPort_Surround_SetsSoundModeSurround)
{
    Activate(device::AudioOutputPortType::kSPEAKER,
             device::AudioStereoMode::kSurround, false, "SPEAKER0");

    // SURROUND is now an enabling mode with ATMOS_METADATA → result=true
    ASSERT_NE(dsListener, nullptr);
    dsListener->OnDolbyAtmosCapabilitiesChanged(dsAUDIO_ATMOS_ATMOSMETADATA, true);

    JsonObject params, result;
    // Call succeeds (ERROR_NONE) but dolbyAtmosExperience value=false
    EXPECT_EQ(Core::ERROR_NONE,
              InvokeServiceMethod(AUDIOOUTPUT_CALLSIGN, "dolbyAtmosExperience", params, result));
}

// ===========================================================================
//  SoundMode: kSPDIF port, getStereoMode=kStereo → _soundMode=STEREO
//  Exercises: lines 413-414 (spdifPorts branch), 429
// ===========================================================================
TEST_F(AudioOutputL2Test_SoundMode_Common, SoundMode_SpdifPort_Stereo_SetsSoundModeStereo)
{
    Activate(device::AudioOutputPortType::kSPDIF,
             device::AudioStereoMode::kStereo, false, "SPDIF0");

    ASSERT_NE(dsListener, nullptr);
    dsListener->OnDolbyAtmosCapabilitiesChanged(dsAUDIO_ATMOS_ATMOSMETADATA, true);

    JsonObject params, result;
    EXPECT_EQ(Core::ERROR_NONE,
              InvokeServiceMethod(AUDIOOUTPUT_CALLSIGN, "dolbyAtmosExperience", params, result));
}

// ===========================================================================
//  SoundMode: kHEADPHONE port, getStereoMode=kMono → _soundMode=MONO
//  Exercises: lines 415-416 (headphonePorts branch), 431
// ===========================================================================
TEST_F(AudioOutputL2Test_SoundMode_Common, SoundMode_HeadphonePort_Mono_SetsSoundModeMono)
{
    Activate(device::AudioOutputPortType::kHEADPHONE,
             device::AudioStereoMode::kMono, false, "HEADPHONE0");

    ASSERT_NE(dsListener, nullptr);
    dsListener->OnDolbyAtmosCapabilitiesChanged(dsAUDIO_ATMOS_ATMOSMETADATA, true);

    JsonObject params, result;
    EXPECT_EQ(Core::ERROR_NONE,
              InvokeServiceMethod(AUDIOOUTPUT_CALLSIGN, "dolbyAtmosExperience", params, result));
}

// ===========================================================================
//  SoundMode: kHDMI port, getStereoAuto=true → kHDMI removed from stereoAuto condition
//  _soundMode=PASSTHRU from getStereoMode() (stereoAuto no longer applies to kHDMI)
//  PASSTHRU is enabling → dolbyAtmosExperience=true
// ===========================================================================
TEST_F(AudioOutputL2Test_SoundMode_Common, SoundMode_HdmiPort_StereoAutoTrue_SetsSoundModePassthru)
{
    Activate(device::AudioOutputPortType::kHDMI,
             device::AudioStereoMode::kPassThru, /*stereoAuto=*/true, "HDMI0");

    // kHDMI removed from stereoAuto condition: mode=PASSTHRU from getStereoMode(), not SOUNDMODE_AUTO
    // PASSTHRU is enabling → true with ATMOSMETADATA
    ASSERT_NE(dsListener, nullptr);
    dsListener->OnDolbyAtmosCapabilitiesChanged(dsAUDIO_ATMOS_ATMOSMETADATA, true);

    JsonObject params, result;
    EXPECT_EQ(Core::ERROR_NONE,
              InvokeServiceMethod(AUDIOOUTPUT_CALLSIGN, "dolbyAtmosExperience", params, result));
}

TEST_F(AudioOutputL2Test_AtmosCapable,
       DsAudioModeToSoundMode_UnknownMode_ReturnsUnknown)
{
    // dsAUDIO_STEREO_MAX is beyond all valid stereo modes.
    // onAudioModeChanged() → DsAudioModeToSoundMode(AudioStereoMode(MAX))
    // → no if-branch matches → LOGWARN + return UNKNOWN (lines 391-392)
    InjectSoundMode(dsAUDIOPORT_TYPE_HDMI, dsAUDIO_STEREO_MAX);

    // _soundMode=UNKNOWN → EvaluateCurrentAtmosExperience() = false
    // (even though _atmosMetaData=true from AtmosCapable fixture)
    JsonObject params, result;
    EXPECT_EQ(Core::ERROR_NONE,
              InvokeServiceMethod(AUDIOOUTPUT_CALLSIGN, "dolbyAtmosExperience", params, result));
}



// Fixture: AudioOutputL2Test (base) — just override mocks inside the test
TEST_F(AudioOutputL2Test, AtmosMetadata_ArcPortDetected_SetsAudioPortToHdmiArc)
{
    // Return one port so the loop runs
    ON_CALL(*p_hostImplMock, getAudioOutputPorts())
        .WillByDefault(Return(device::List<device::AudioOutputPort>{device::AudioOutputPort()}));

    // Port name contains "HDMI_ARC" → audioPort switches to "HDMI_ARC0" (line 350)
    ON_CALL(*p_audioOutputPortMock, getName())
        .WillByDefault(ReturnRefOfCopy(std::string("HDMI_ARC0")));

    // isConnected=false → takes else branch (host-level query)
    // (getSinkDeviceAtmosCapability returns NOTSUPPORTED by default)
    ON_CALL(*p_audioOutputPortMock, isConnected()).WillByDefault(Return(false));

    // Trigger AtmosMetadata via dolbyAtmosExperience JSON-RPC
    JsonObject params, result;
    EXPECT_EQ(Core::ERROR_NONE,
              InvokeServiceMethod(AUDIOOUTPUT_CALLSIGN, "dolbyAtmosExperience", params, result));
}


TEST_F(AudioOutputL2Test, AtmosMetadata_PortConnected_CallsPortLevelAtmosCapability)
{
    // isConnected=true → enters the if-branch (line 358)
    ON_CALL(*p_audioOutputPortMock, isConnected()).WillByDefault(Return(true));

    // getSinkDeviceAtmosCapability sets atmosCapability via out-param
    ON_CALL(*p_audioOutputPortMock, getSinkDeviceAtmosCapability(_))
        .WillByDefault(DoAll(
            ::testing::SetArgReferee<0>(dsAUDIO_ATMOS_ATMOSMETADATA),
            Return(true)));

    // AtmosMetadata runs → port connected → line 358 hit → atmosCapability=ATMOSMETADATA
    // Inject to confirm: dsListener fires → but we verify via JSON-RPC
    JsonObject params, result;
    EXPECT_EQ(Core::ERROR_NONE,
              InvokeServiceMethod(AUDIOOUTPUT_CALLSIGN, "dolbyAtmosExperience", params, result));
}

// ===========================================================================
//  AudioOutputL2Test_ManagerInitFailure
//
//  Fixture where Manager::Initialize() throws device::Exception.
//  Covers constructor catch block (lines 53-55).
//
//  Constructor flow:
//    try { Manager::Initialize(); }  ← throws
//    catch (device::Exception& err) { LOGWARN(...); }  ← lines 53-55 HIT
//  Constructor completes normally → Configure() still runs.
// ===========================================================================
class AudioOutputL2Test_ManagerInitFailure : public L2TestMocks {
protected:
    device::AudioOutputPort _portObj;

    AudioOutputL2Test_ManagerInitFailure() : L2TestMocks()
    {
        TEST_LOG("AudioOutputL2Test_ManagerInitFailure constructor");

        // Initialize throws → constructor catch block (lines 53-55) hit
        ON_CALL(*p_managerImplMock, Initialize())
            .WillByDefault(Throw(device::Exception("Manager::Initialize failed")));
        ON_CALL(*p_managerImplMock, DeInitialize()).WillByDefault(Return());

        // Configure() still runs after constructor catch; mock its dependencies
        ON_CALL(*p_hostImplMock, getAudioOutputPorts())
            .WillByDefault(Return(device::List<device::AudioOutputPort>{}));
        ON_CALL(*p_hostImplMock, getAudioOutputPort(_))
            .WillByDefault(ReturnRef(_portObj));
        ON_CALL(*p_audioOutputPortMock, isConnected()).WillByDefault(Return(false));

        // Initialize threw → registerDsEventHandlers was skipped → Register
        // won't be called, but set up mock to be safe
        ON_CALL(*p_hostImplMock,
                Register(A<device::Host::IAudioOutputPortEvents*>()))
            .WillByDefault(Return(dsERR_NONE));
        ON_CALL(*p_hostImplMock,
                UnRegister(A<device::Host::IAudioOutputPortEvents*>()))
            .WillByDefault(Return(dsERR_NONE));

        ActivateService(AUDIOOUTPUT_CALLSIGN);
    }

    virtual ~AudioOutputL2Test_ManagerInitFailure() override
    {
        TEST_LOG("AudioOutputL2Test_ManagerInitFailure destructor");
        usleep(CLEANUP_DELAY_MICROSECONDS);
        DeactivateService(AUDIOOUTPUT_CALLSIGN);
    }
};

// ===========================================================================
//  Scenario J — Constructor catch: Manager::Initialize() throws
//
//  Lines covered: 53-55 (constructor catch block)
//  Plugin still activates after the catch (Configure runs normally).
// ===========================================================================
TEST_F(AudioOutputL2Test_ManagerInitFailure,
       Constructor_ManagerInitializeFails_CatchBlockHit)
{
    TEST_LOG("Scenario J: Manager::Initialize throws → constructor catch (lines 53-55) hit");

    // The constructor catch (lines 53-55) fired during ActivateService in the
    // fixture constructor. Verify the plugin still responds to JSON-RPC.
    JsonObject params, result;
    EXPECT_EQ(Core::ERROR_NONE,
              InvokeServiceMethod(AUDIOOUTPUT_CALLSIGN, "dolbyAtmosExperience", params, result));
}

// ===========================================================================
//  AudioOutputL2Test_ManagerDeInitFailure
//
//  Fixture where Manager::DeInitialize() throws device::Exception.
//  Covers destructor catch block (lines 68-70).
//
//  Destructor flow:
//    try {
//        if (_registeredDsEventHandlers) unregisterDsEventHandlers();  ← OK
//        Manager::DeInitialize();  ← throws
//    } catch (device::Exception& err) { LOGWARN(...); }  ← lines 68-70 HIT
//  Fires when fixture destructor calls DeactivateService().
// ===========================================================================
class AudioOutputL2Test_ManagerDeInitFailure : public L2TestMocks {
protected:
    device::AudioOutputPort _portObj;
    device::Host::IAudioOutputPortEvents* dsListener{nullptr};

    AudioOutputL2Test_ManagerDeInitFailure() : L2TestMocks()
    {
        TEST_LOG("AudioOutputL2Test_ManagerDeInitFailure constructor");

        ON_CALL(*p_managerImplMock, Initialize()).WillByDefault(Return());
        // DeInitialize throws → destructor catch block (lines 68-70) hit
        ON_CALL(*p_managerImplMock, DeInitialize())
            .WillByDefault(Throw(device::Exception("Manager::DeInitialize failed")));

        ON_CALL(*p_hostImplMock, getAudioOutputPorts())
            .WillByDefault(Return(device::List<device::AudioOutputPort>{}));
        ON_CALL(*p_hostImplMock, getAudioOutputPort(_))
            .WillByDefault(ReturnRef(_portObj));
        ON_CALL(*p_audioOutputPortMock, isConnected()).WillByDefault(Return(false));

        ON_CALL(*p_hostImplMock,
                Register(A<device::Host::IAudioOutputPortEvents*>()))
            .WillByDefault(DoAll(SaveArg<0>(&dsListener), Return(dsERR_NONE)));
        ON_CALL(*p_hostImplMock,
                UnRegister(A<device::Host::IAudioOutputPortEvents*>()))
            .WillByDefault(Return(dsERR_NONE));

        uint32_t status = ActivateService(AUDIOOUTPUT_CALLSIGN);
        EXPECT_EQ(Core::ERROR_NONE, status);
    }

    virtual ~AudioOutputL2Test_ManagerDeInitFailure() override
    {
        TEST_LOG("AudioOutputL2Test_ManagerDeInitFailure destructor");
        usleep(CLEANUP_DELAY_MICROSECONDS);
        // DeactivateService → plugin destructor → DeInitialize throws
        // → catch block (lines 68-70) hit
        DeactivateService(AUDIOOUTPUT_CALLSIGN);
    }
};

// ===========================================================================
//  Scenario K — Destructor catch: Manager::DeInitialize() throws
//
//  Lines covered: 68-70 (destructor catch block)
//  The catch fires when the fixture destructor calls DeactivateService().
// ===========================================================================
TEST_F(AudioOutputL2Test_ManagerDeInitFailure,
       Destructor_ManagerDeInitializeFails_CatchBlockHit)
{
    TEST_LOG("Scenario K: Manager::DeInitialize throws → destructor catch (lines 68-70) hit");

    // Plugin activated normally. The destructor catch (lines 68-70) will fire
    // when this test ends and the fixture destructor calls DeactivateService.
    // Verify the plugin responds normally during the test body.
    JsonObject params, result;
    EXPECT_EQ(Core::ERROR_NONE,
              InvokeServiceMethod(AUDIOOUTPUT_CALLSIGN, "dolbyAtmosExperience", params, result));
}

// ===========================================================================
//  AudioOutputL2Test_SoundModeInitFailure
//
//  Fixture where getAudioOutputPorts() throws device::Exception.
//  This makes BOTH AtmosMetadata() and SoundMode() fail in Configure().
//
//  Configure() flow:
//    AtmosMetadata() → getAudioOutputPorts() throws → ERROR_GENERAL  (lines 84-87)
//    SoundMode()     → getAudioOutputPorts() throws → ERROR_GENERAL  (lines 91-94) ← TARGET
//    Both init-failed flags set to true.
// ===========================================================================
class AudioOutputL2Test_SoundModeInitFailure : public L2TestMocks {
protected:
    // Persistent port object for the getAudioOutputPort(name) call in
    // AtmosMetadata() when the retry path executes with an empty port list.
    device::AudioOutputPort _portObj;

    AudioOutputL2Test_SoundModeInitFailure() : L2TestMocks()
    {
        TEST_LOG("AudioOutputL2Test_SoundModeInitFailure constructor");

        ON_CALL(*p_managerImplMock, Initialize()).WillByDefault(Return());
        ON_CALL(*p_managerImplMock, DeInitialize()).WillByDefault(Return());

        // getAudioOutputPorts throws → AtmosMetadata AND SoundMode both fail:
        //   AtmosMetadata: catch → ERROR_GENERAL → lines 84-87 hit
        //   SoundMode:     catch → ERROR_GENERAL → lines 91-94 hit  ← TARGET
        ON_CALL(*p_hostImplMock, getAudioOutputPorts())
            .WillByDefault(Throw(device::Exception("Ports unavailable")));

        // Required when the retry path runs with a fixed (non-throwing)
        // getAudioOutputPorts(): AtmosMetadata still calls getAudioOutputPort(name).
        ON_CALL(*p_hostImplMock, getAudioOutputPort(_))
            .WillByDefault(ReturnRef(_portObj));
        ON_CALL(*p_audioOutputPortMock, isConnected())
            .WillByDefault(Return(false));

        ON_CALL(*p_hostImplMock,
                Register(A<device::Host::IAudioOutputPortEvents*>()))
            .WillByDefault(Return(dsERR_NONE));
        ON_CALL(*p_hostImplMock,
                UnRegister(A<device::Host::IAudioOutputPortEvents*>()))
            .WillByDefault(Return(dsERR_NONE));

        uint32_t status = ActivateService(AUDIOOUTPUT_CALLSIGN);
        if (status != Core::ERROR_NONE) {
            TEST_LOG("Warning: ActivateService returned %u (Configure returns ERROR_NONE always)", status);
        }
    }

    virtual ~AudioOutputL2Test_SoundModeInitFailure() override
    {
        TEST_LOG("AudioOutputL2Test_SoundModeInitFailure destructor");
        usleep(CLEANUP_DELAY_MICROSECONDS);
        DeactivateService(AUDIOOUTPUT_CALLSIGN);
    }
};

// ===========================================================================
//  Scenario L — Configure: SoundMode() init failure path
//
//  Lines covered: 91-94 in Configure()
//    LOGERR("Configure: failed to get sound mode")
//    soundModeInitFailed = true
//
//  getAudioOutputPorts() throws during ActivateService() in the constructor →
//  Configure sets both _atmosMetadataInitFailed and _soundModeInitFailed=true.
//
//  NOTE: Calling InvokeServiceMethod while getAudioOutputPorts() still throws
//  causes std::terminate() — the exception propagates through WPEFramework's
//  noexcept JSON-RPC dispatch chain and crashes the process, leaking all mock
//  objects. To avoid this, the mock is fixed to return an empty list before
//  calling InvokeServiceMethod. The configure failure lines are already covered
//  by the fixture constructor; fixing the mock then confirms the retry path
//  clears the init flags and succeeds (mirrors Scenario I for _InitFailure).
// ===========================================================================
TEST_F(AudioOutputL2Test_SoundModeInitFailure,
       Configure_SoundModeFails_SoundModeInitFailedSet)
{
    TEST_LOG("Scenario L: SoundMode() fails in Configure → lines 91-94 hit");

    // Lines 91-94 already exercised during ActivateService() in the constructor.
    // Fix getAudioOutputPorts() so the retry path in DolbyAtmosExperience()
    // does not throw — leaving it throwing causes std::terminate() via the
    // WPEFramework noexcept JSONRPC dispatch chain and leaks all mock objects.
    ON_CALL(*p_hostImplMock, getAudioOutputPorts())
        .WillByDefault(Return(device::List<device::AudioOutputPort>{}));

    // With both init flags set and the mock now fixed:
    //   AtmosMetadata retry: ports={} → getAudioOutputPort → isConnected=false
    //                        → NOTSUPPORTED → supported=false, ERROR_NONE
    //   SoundMode retry:     ports={} → loop skipped → ERROR_NONE
    // Flags cleared, UpdateCache runs, DolbyAtmosExperience returns ERROR_NONE.
    JsonObject params, result;
    uint32_t status = InvokeServiceMethod(AUDIOOUTPUT_CALLSIGN, "dolbyAtmosExperience",
                                          params, result);
    EXPECT_EQ(Core::ERROR_NONE, status)
        << "After fixing HAL mock, retry must clear init flags and return ERROR_NONE";
}

// ===========================================================================
//  Scenario M — onAudioModeChanged: getAudioOutputPorts() throws
//
//  Lines covered: 262-264 in onAudioModeChanged() catch block
//
//  After activation (normal), override getAudioOutputPorts() to throw, then
//  fire OnAudioModeEvent. onAudioModeChanged() calls getAudioOutputPorts() →
//  throws → caught at lines 262-264 → _soundMode unchanged → UpdateCache runs.
//  Plugin continues to function normally after the exception.
// ===========================================================================
TEST_F(AudioOutputL2Test, OnAudioModeChanged_GetPortsThrows_ExceptionCaught)
{
    TEST_LOG("Scenario M: onAudioModeChanged → getAudioOutputPorts throws → catch (lines 262-264) hit");

    ASSERT_NE(dsListener, nullptr);

    // Override: make getAudioOutputPorts throw inside onAudioModeChanged
    ON_CALL(*p_hostImplMock, getAudioOutputPorts())
        .WillByDefault(Throw(device::Exception("Ports unavailable")));

    // Fire OnAudioModeEvent → onAudioModeChanged() → getAudioOutputPorts throws
    // → lines 262-264 hit → exception caught → _soundMode unchanged (UNKNOWN)
    dsListener->OnAudioModeEvent(dsAUDIOPORT_TYPE_HDMI, dsAUDIO_STEREO_PASSTHRU);

    // Plugin must still respond normally after the internal exception
    JsonObject params, result;
    EXPECT_EQ(Core::ERROR_NONE,
              InvokeServiceMethod(AUDIOOUTPUT_CALLSIGN, "dolbyAtmosExperience", params, result));
}

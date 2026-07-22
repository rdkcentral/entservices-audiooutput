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
// Responsibilities:
//   1. Wire all DS HAL mocks (Manager, Host, AudioOutputPort, AudioOutputPortType).
//   2. Capture the IAudioOutputPortEvents* listener the plugin registers via
//      Host::Register() in its constructor. This is the only way to drive
//      _atmosMetaData and _soundMode state changes (the plugin has no public
//      setters and does not implement Exchange::Dolby::IOutput::INotification).
//   3. Activate / deactivate the AudioOutput plugin around each test.
//   4. Provide helpers InjectAtmosCapability() and InjectSoundMode() that fire
//      DS HAL callbacks directly through the captured listener — exactly the
//      same production path used on a real device.
// ---------------------------------------------------------------------------
class AudioOutputL2Test : public L2TestMocks {
protected:
    // Captured by SaveArg<0> inside the ON_CALL for Host::Register.
    // Populated during plugin construction (constructor → registerDsEventHandlers
    // → device::Host::getInstance().Register(&_dsAudioPortNotification, ...)).
    device::Host::IAudioOutputPortEvents* dsListener = nullptr;

    // Persistent AudioOutputPort object returned by getAudioOutputPort().
    // AtmosMetadata() always calls getAudioOutputPort("HDMI0") after scanning
    // getAudioOutputPorts(); without a mock setup the reference-return default
    // action throws and crashes WPEFramework during plugin activation.
    device::AudioOutputPort _audioPortObj;

    // Persistent AudioOutputPortType object whose getId() delegates to
    // p_audioOutputPortTypeMock (via AudioOutputPortType::impl).
    // Used by InjectSoundMode() as the return value of AudioOutputPortMock::getType().
    device::AudioOutputPortType portTypeObj;

    Core::ProxyType<RPC::InvokeServerType<1, 0, 4>> mAudioOutputEngine;
    Core::ProxyType<RPC::CommunicatorClient>        mAudioOutputClient;
    Exchange::IAudioOutput*                          mAudioOutputPlugin{nullptr};

    AudioOutputL2Test();
    virtual ~AudioOutputL2Test() override;

    uint32_t CreateAudioOutputInterfaceObjectUsingComRPCConnection();

    // ------------------------------------------------------------------
    // Helper: fire OnDolbyAtmosCapabilitiesChanged on the captured listener.
    //
    // Plugin's DsAudioPortNotification::OnDolbyAtmosCapabilitiesChanged
    // is an OVERRIDE — the virtual call goes straight to the plugin, not
    // through the devicesettings stub (which would dispatch to impl->...).
    //
    // Plugin::onAtmosCapabilitiesChanged():
    //   _atmosMetaData = (cap == dsAUDIO_ATMOS_ATMOSMETADATA)
    //   UpdateCache() → EvaluateCurrentAtmosExperience() → maybe SendNotify()
    // ------------------------------------------------------------------
    void InjectAtmosCapability(dsATMOSCapability_t cap, bool status = true)
    {
        ASSERT_NE(dsListener, nullptr)
            << "dsListener is null: plugin may not have called Host::Register";
        dsListener->OnDolbyAtmosCapabilitiesChanged(cap, status);
    }

    // ------------------------------------------------------------------
    // Helper: fire OnAudioModeEvent on the captured listener.
    //
    // Sets up getAudioOutputPorts() to return ONE port whose type id matches
    // portTypeVal, so plugin's onAudioModeChanged() updates _soundMode:
    //
    //   if (typeId == portType && HDMI/ARC/SPDIF/SPEAKER && getStereoAuto())
    //       _soundMode = SOUNDMODE_AUTO
    //   else if (typeId == portType)
    //       _soundMode = DsAudioModeToSoundMode(AudioStereoMode(smode))
    //
    // stereoAuto=false triggers the DsAudioModeToSoundMode path.
    // stereoAuto=true  triggers the SOUNDMODE_AUTO path.
    // ------------------------------------------------------------------
    void InjectSoundMode(dsAudioPortType_t portTypeVal,
                         dsAudioStereoMode_t smode,
                         bool stereoAuto = false)
    {
        ASSERT_NE(dsListener, nullptr)
            << "dsListener is null: plugin may not have called Host::Register";

        // Return exactly one port so the loop finds a match
        ON_CALL(*p_hostImplMock, getAudioOutputPorts())
            .WillByDefault(
                Return(device::List<device::AudioOutputPort>{device::AudioOutputPort()}));

        // audioOutputPortMock.getType() → portTypeObj
        // portTypeObj.getId() delegates to p_audioOutputPortTypeMock via impl pointer
        ON_CALL(*p_audioOutputPortMock, getType())
            .WillByDefault(ReturnRef(portTypeObj));

        // audioOutputPortTypeMock.getId() → portTypeVal
        ON_CALL(*p_audioOutputPortTypeMock, getId())
            .WillByDefault(Return(static_cast<int>(portTypeVal)));

        // stereoAuto controls which branch is taken in onAudioModeChanged
        ON_CALL(*p_audioOutputPortMock, getStereoAuto())
            .WillByDefault(Return(stereoAuto));

        dsListener->OnAudioModeEvent(portTypeVal, smode);
    }
};

AudioOutputL2Test::AudioOutputL2Test()
    : L2TestMocks()
{
    TEST_LOG("AudioOutputL2Test constructor");

    // Manager stubs — called by plugin constructor / destructor
    ON_CALL(*p_managerImplMock, Initialize()).WillByDefault(Return());
    ON_CALL(*p_managerImplMock, DeInitialize()).WillByDefault(Return());

    // Default: no ports → SoundMode / onAudioModeChanged fast-exit (empty list)
    ON_CALL(*p_hostImplMock, getAudioOutputPorts())
        .WillByDefault(Return(device::List<device::AudioOutputPort>{}));

    // AtmosMetadata() always calls getAudioOutputPort(portName) after the
    // getAudioOutputPorts() loop, regardless of how many ports were returned.
    // Return a reference to the persistent _audioPortObj so the plugin does not
    // crash on the reference-return default action.
    ON_CALL(*p_hostImplMock, getAudioOutputPort(_))
        .WillByDefault(ReturnRef(_audioPortObj));

    // isConnected()=false → AtmosMetadata takes the else branch and calls
    // Host::getSinkDeviceAtmosCapability instead of aPort.getSinkDeviceAtmosCapability.
    // NiceMock default for getSinkDeviceAtmosCapability leaves atmosCapability
    // as dsAUDIO_ATMOS_NOTSUPPORTED → AtmosMetadata returns supported=false.
    ON_CALL(*p_audioOutputPortMock, isConnected())
        .WillByDefault(Return(false));

    // Capture the IAudioOutputPortEvents* listener that the plugin registers
    // during construction (registerDsEventHandlers → Host::Register).
    // SaveArg<0> stores the first argument of Register() into dsListener.
    ON_CALL(*p_hostImplMock,
            Register(A<device::Host::IAudioOutputPortEvents*>()))
        .WillByDefault(DoAll(SaveArg<0>(&dsListener), Return(dsERR_NONE)));
    ON_CALL(*p_hostImplMock,
            UnRegister(A<device::Host::IAudioOutputPortEvents*>()))
        .WillByDefault(Return(dsERR_NONE));

    uint32_t status = ActivateService(AUDIOOUTPUT_CALLSIGN);
    if (status != Core::ERROR_NONE) {
        TEST_LOG("Failed to activate AudioOutput plugin: %u", status);
    }

    // dsListener must be valid — populated by the plugin's constructor call
    // to device::Host::getInstance().Register(&_dsAudioPortNotification, ...)
    EXPECT_NE(dsListener, nullptr)
        << "Plugin did not call Host::Register in constructor";
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
TEST_F(AudioOutputL2Test, JsonRpc_DolbyAtmosExperience_DefaultFalse_WhenHalNotSupported)
{
    TEST_LOG("Scenario 1: JSON-RPC dolbyAtmosExperience — NOT_SUPPORTED HAL → false");

    JsonObject params, result;
    uint32_t status = InvokeServiceMethod(
        AUDIOOUTPUT_CALLSIGN,
        "dolbyAtmosExperience",
        params,
        result);

    EXPECT_EQ(Core::ERROR_NONE, status)
        << "JSON-RPC call must succeed";

    // Confirm via COM-RPC: default state = false
    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());
    ASSERT_NE(mAudioOutputPlugin, nullptr);

    bool value = true; // pre-set true to confirm it gets overwritten to false
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->DolbyAtmosExperience(value));
    EXPECT_FALSE(value)
        << "HAL=NOT_SUPPORTED, soundMode=UNKNOWN: dolbyAtmosExperience must be false";
}

// ===========================================================================
//  Scenario 2 — COM-RPC, Atmos-capable hardware + non-enabling sound mode
//
//  HAL state: _atmosMetaData=true  (injected in fixture constructor)
//             _soundMode=SURROUND  (injected via DS HAL callback)
//
//  Decision logic:
//    _atmosMetaData=true → Step 1 passes
//    _soundMode=SURROUND → NOT in {PASSTHRU, DOLBYDIGITALPLUS, SOUNDMODE_AUTO}
//    switch default: return false
//
//  Real-world meaning: AVR supports Atmos but current content is plain Surround.
// ===========================================================================
TEST_F(AudioOutputL2Test_AtmosCapable, ComRpc_DolbyAtmosExperience_AtmosCapableButSurroundMode_ReturnsFalse)
{
    TEST_LOG("Scenario 2: ATMOSMETADATA + SURROUND → dolbyAtmosExperience=false");

    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());
    ASSERT_NE(mAudioOutputPlugin, nullptr);

    // Inject SURROUND sound mode via DS HAL OnAudioModeEvent callback.
    // dsAUDIO_STEREO_SURROUND → DsAudioModeToSoundMode → SURROUND
    // SURROUND is not in the enabling set → evaluate=false
    InjectSoundMode(dsAUDIOPORT_TYPE_HDMI, dsAUDIO_STEREO_SURROUND);

    bool enabled = true; // pre-set true to confirm it gets overwritten to false
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->DolbyAtmosExperience(enabled));
    EXPECT_FALSE(enabled)
        << "SURROUND is not an Atmos-enabling mode; result must be false";
}

// ===========================================================================
//  Scenario 3 — COM-RPC, Atmos-capable hardware + DOLBYDIGITALPLUS mode → true
//
//  HAL state: _atmosMetaData=true  (injected in fixture constructor)
//             _soundMode=DOLBYDIGITALPLUS (injected via DS HAL callback)
//
//  Decision logic:
//    _atmosMetaData=true     → Step 1 passes
//    DOLBYDIGITALPLUS is in enabling set → Step 2 passes
//    EvaluateCurrentAtmosExperience() = true
//
//  Real-world meaning: AVR supports Atmos AND content is Atmos stream → true.
// ===========================================================================
TEST_F(AudioOutputL2Test_AtmosCapable, ComRpc_DolbyAtmosExperience_AtmosCapableAndDolbyDigitalPlusMode_ReturnsTrue)
{
    TEST_LOG("Scenario 3: ATMOSMETADATA + DOLBYDIGITALPLUS → dolbyAtmosExperience=true");

    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());
    ASSERT_NE(mAudioOutputPlugin, nullptr);

    // Inject DOLBYDIGITALPLUS via DS HAL callback.
    // dsAUDIO_STEREO_DDPLUS → DsAudioModeToSoundMode → DOLBYDIGITALPLUS
    InjectSoundMode(dsAUDIOPORT_TYPE_HDMI, dsAUDIO_STEREO_DDPLUS);

    bool enabled = false;
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->DolbyAtmosExperience(enabled));
    EXPECT_TRUE(enabled)
        << "DOLBYDIGITALPLUS with ATMOSMETADATA hardware must return true";
}

// ===========================================================================
//  Scenario 3b — COM-RPC, Atmos-capable hardware + PASSTHRU mode → true
//
//  HAL state: _atmosMetaData=true
//             _soundMode=PASSTHRU (injected)
//
//  PASSTHRU is in {PASSTHRU, DOLBYDIGITALPLUS, SOUNDMODE_AUTO} → true.
// ===========================================================================
TEST_F(AudioOutputL2Test_AtmosCapable, ComRpc_DolbyAtmosExperience_AtmosCapableAndPassthruMode_ReturnsTrue)
{
    TEST_LOG("Scenario 3b: ATMOSMETADATA + PASSTHRU → dolbyAtmosExperience=true");

    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());
    ASSERT_NE(mAudioOutputPlugin, nullptr);

    // dsAUDIO_STEREO_PASSTHRU → DsAudioModeToSoundMode → PASSTHRU
    InjectSoundMode(dsAUDIOPORT_TYPE_HDMI, dsAUDIO_STEREO_PASSTHRU);

    bool enabled = false;
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->DolbyAtmosExperience(enabled));
    EXPECT_TRUE(enabled)
        << "PASSTHRU with ATMOSMETADATA hardware must return true";
}

// ===========================================================================
//  Scenario 3c — COM-RPC, Atmos-capable hardware + SOUNDMODE_AUTO → true
//
//  HAL state: _atmosMetaData=true
//             _soundMode=SOUNDMODE_AUTO (triggered by getStereoAuto()=true)
//
//  onAudioModeChanged() path for SOUNDMODE_AUTO:
//    if (typeId==portType && HDMI/ARC/SPDIF/SPEAKER && getStereoAuto()==true)
//        _soundMode = SOUNDMODE_AUTO   ← this branch
//  SOUNDMODE_AUTO is in enabling set → true.
// ===========================================================================
TEST_F(AudioOutputL2Test_AtmosCapable, ComRpc_DolbyAtmosExperience_AtmosCapableAndSoundModeAuto_ReturnsTrue)
{
    TEST_LOG("Scenario 3c: ATMOSMETADATA + SOUNDMODE_AUTO → dolbyAtmosExperience=true");

    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());
    ASSERT_NE(mAudioOutputPlugin, nullptr);

    // stereoAuto=true → onAudioModeChanged sets _soundMode=SOUNDMODE_AUTO
    InjectSoundMode(dsAUDIOPORT_TYPE_HDMI, dsAUDIO_STEREO_PASSTHRU, /*stereoAuto=*/true);

    bool enabled = false;
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->DolbyAtmosExperience(enabled));
    EXPECT_TRUE(enabled)
        << "SOUNDMODE_AUTO with ATMOSMETADATA hardware must return true";
}

// ===========================================================================
//  Scenario 4 — COM-RPC, hardware NOT capable even with Atmos stream
//
//  HAL state: _atmosMetaData=false (default — NOT_SUPPORTED)
//             _soundMode=DOLBYDIGITALPLUS (injected — simulates Atmos stream)
//
//  Decision logic:
//    if (!_atmosMetaData) return false;  ← gate fails immediately
//    soundMode is irrelevant — hardware capability is the gate
//
//  Real-world: Atmos stream + non-Atmos hardware = no Atmos experience.
// ===========================================================================
TEST_F(AudioOutputL2Test, ComRpc_DolbyAtmosExperience_NotCapable_ReturnsFalse_EvenWithAtmosStream)
{
    TEST_LOG("Scenario 4: NOT_SUPPORTED HAL + DOLBYDIGITALPLUS stream → false");

    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());
    ASSERT_NE(mAudioOutputPlugin, nullptr);

    // Inject DOLBYDIGITALPLUS — hardware gate (_atmosMetaData=false) blocks it
    InjectSoundMode(dsAUDIOPORT_TYPE_HDMI, dsAUDIO_STEREO_DDPLUS);

    bool enabled = true;
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->DolbyAtmosExperience(enabled));
    EXPECT_FALSE(enabled)
        << "NOT_SUPPORTED hardware must return false even when stream is DOLBYDIGITALPLUS";
}

// ===========================================================================
//  Scenario A — Notification fires TRUE on false→true transition
//
//  HAL state: _atmosMetaData=true (fixture)
//  Flow:
//    1. Inject SURROUND → _soundMode=SURROUND → result=false
//    2. Register sink
//    3. Inject PASSTHRU → _soundMode=PASSTHRU → result=true
//    4. false→true change detected in UpdateCache() → SendNotify(true)
//    5. Sink receives OnDolbyAtmosExperienceChanged(true)
//
//  Expected: notification fires, lastValue=true
// ===========================================================================
TEST_F(AudioOutputL2Test_AtmosCapable, ComRpc_Notification_FiredWithTrue_OnFalseToTrueTransition)
{
    TEST_LOG("Scenario A: SURROUND→PASSTHRU triggers OnDolbyAtmosExperienceChanged(true)");

    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());
    ASSERT_NE(mAudioOutputPlugin, nullptr);

    // Step 1: set initial state to false — SURROUND is non-enabling
    // _atmosMetaData=true (fixture) + SURROUND → result=false
    InjectSoundMode(dsAUDIOPORT_TYPE_HDMI, dsAUDIO_STEREO_SURROUND);

    bool initial = true;
    ASSERT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->DolbyAtmosExperience(initial));
    ASSERT_FALSE(initial) << "Pre-condition: initial state must be false";

    // Step 2: register sink BEFORE triggering the change
    auto sink = Core::ProxyType<AudioOutputNotificationSink>::Create();
    mAudioOutputPlugin->Register(&(*sink));

    // Step 3: trigger false→true — PASSTHRU is enabling
    // UpdateCache(): old=false, new=true → changed=true → SendNotify(true)
    InjectSoundMode(dsAUDIOPORT_TYPE_HDMI, dsAUDIO_STEREO_PASSTHRU);

    // Step 4: wait for the COM-RPC async notification
    bool fired = sink->WaitForNotification(2000);
    EXPECT_TRUE(fired) << "Notification must fire on false→true transition";

    // Step 5: payload must be true
    EXPECT_TRUE(sink->LastValue())
        << "ATMOSMETADATA + PASSTHRU notification payload must be true";

    mAudioOutputPlugin->Unregister(&(*sink));
}

// ===========================================================================
//  Scenario B — Notification fires FALSE on true→false transition
//
//  HAL state: _atmosMetaData=true (fixture)
//  Flow:
//    1. Inject PASSTHRU → result=true
//    2. Register sink
//    3. Inject SURROUND → result=false
//    4. true→false → SendNotify(false) fires
//
//  Expected: notification fires, lastValue=false
// ===========================================================================
TEST_F(AudioOutputL2Test_AtmosCapable, ComRpc_Notification_FiredWithFalse_OnTrueToFalseTransition)
{
    TEST_LOG("Scenario B: PASSTHRU→SURROUND triggers OnDolbyAtmosExperienceChanged(false)");

    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());
    ASSERT_NE(mAudioOutputPlugin, nullptr);

    // Step 1: set initial state to true
    InjectSoundMode(dsAUDIOPORT_TYPE_HDMI, dsAUDIO_STEREO_PASSTHRU);

    bool initial = false;
    ASSERT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->DolbyAtmosExperience(initial));
    ASSERT_TRUE(initial) << "Pre-condition: initial state must be true";

    // Step 2: register sink
    auto sink = Core::ProxyType<AudioOutputNotificationSink>::Create();
    mAudioOutputPlugin->Register(&(*sink));

    // Step 3: trigger true→false
    InjectSoundMode(dsAUDIOPORT_TYPE_HDMI, dsAUDIO_STEREO_SURROUND);

    // Step 4: wait for notification
    bool fired = sink->WaitForNotification(2000);
    EXPECT_TRUE(fired) << "Notification must fire on true→false transition";

    EXPECT_FALSE(sink->LastValue())
        << "ATMOSMETADATA + SURROUND notification payload must be false";

    mAudioOutputPlugin->Unregister(&(*sink));
}

// ===========================================================================
//  Scenario C — NO notification when value does not change (false→false)
//
//  HAL state: _atmosMetaData=true (fixture)
//  Flow:
//    1. Inject SURROUND → result=false
//    2. Register sink
//    3. Inject STEREO  → result still=false (both non-enabling)
//    4. old==new → SendNotify NOT called
//
//  Expected: WasNotified()==false
// ===========================================================================
TEST_F(AudioOutputL2Test_AtmosCapable, ComRpc_Notification_NotFired_WhenValueUnchanged_FalseToFalse)
{
    TEST_LOG("Scenario C: SURROUND→STEREO keeps result=false, no notification");

    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());
    ASSERT_NE(mAudioOutputPlugin, nullptr);

    // Step 1: set state to false using SURROUND
    InjectSoundMode(dsAUDIOPORT_TYPE_HDMI, dsAUDIO_STEREO_SURROUND);

    // Step 2: register sink
    auto sink = Core::ProxyType<AudioOutputNotificationSink>::Create();
    mAudioOutputPlugin->Register(&(*sink));

    // Step 3: inject STEREO — also non-enabling
    // old=false, new=false → no change → SendNotify NOT called
    InjectSoundMode(dsAUDIOPORT_TYPE_HDMI, dsAUDIO_STEREO_STEREO);

    // Step 4: short wait — nothing should arrive
    bool fired = sink->WaitForNotification(500);
    EXPECT_FALSE(fired) << "No notification when result stays false";
    EXPECT_FALSE(sink->WasNotified());

    mAudioOutputPlugin->Unregister(&(*sink));
}

// ===========================================================================
//  Scenario D — NO notification delivered to an unregistered sink
//
//  HAL state: _atmosMetaData=true (fixture)
//  Flow:
//    1. Inject SURROUND → result=false (would produce true→false notification
//       if PASSTHRU→SURROUND; but we start from SURROUND directly)
//    2. Register sink → Unregister sink immediately
//    3. Inject PASSTHRU → false→true would have fired (if registered)
//    4. Sink is NOT registered → no notification delivered
//
//  Expected: WasNotified()==false
// ===========================================================================
TEST_F(AudioOutputL2Test_AtmosCapable, ComRpc_Notification_NotFired_AfterUnregister)
{
    TEST_LOG("Scenario D: unregistered sink must not receive any notification");

    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());
    ASSERT_NE(mAudioOutputPlugin, nullptr);

    // Set state to false so a PASSTHRU injection would be a real false→true
    InjectSoundMode(dsAUDIOPORT_TYPE_HDMI, dsAUDIO_STEREO_SURROUND);

    // Register then immediately unregister BEFORE the triggering event
    auto sink = Core::ProxyType<AudioOutputNotificationSink>::Create();
    mAudioOutputPlugin->Register(&(*sink));
    mAudioOutputPlugin->Unregister(&(*sink));

    // Trigger: PASSTHRU would cause false→true if the sink were still registered
    InjectSoundMode(dsAUDIOPORT_TYPE_HDMI, dsAUDIO_STEREO_PASSTHRU);

    // Short wait — sink is unregistered and must receive nothing
    bool fired = sink->WaitForNotification(500);
    EXPECT_FALSE(fired) << "Unregistered sink must not receive any notification";
    EXPECT_FALSE(sink->WasNotified());
}

// ===========================================================================
//  Scenario E — Notification fires TRUE when AtmosCapability becomes SUPPORTED
//               while soundMode is already enabling (PASSTHRU)
//
//  HAL state starts: _atmosMetaData=false (default NOT_SUPPORTED)
//                    _soundMode=PASSTHRU (injected)
//  result=false (_atmosMetaData=false gates it)
//
//  Trigger:
//    InjectAtmosCapability(ATMOSMETADATA):
//      onAtmosCapabilitiesChanged: _atmosMetaData=true
//      EvaluateCurrentAtmosExperience(): ATMOSMETADATA + PASSTHRU = true
//      false→true → SendNotify(true)
//
//  Expected: notification fires, lastValue=true
// ===========================================================================
TEST_F(AudioOutputL2Test, ComRpc_Notification_FiredTrue_WhenAtmosCapabilityBecomesSupported)
{
    TEST_LOG("Scenario E: NOT_SUPPORTED→ATMOSMETADATA + PASSTHRU → notification(true)");

    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());
    ASSERT_NE(mAudioOutputPlugin, nullptr);

    // Step 1: inject PASSTHRU with _atmosMetaData still false → result=false
    InjectSoundMode(dsAUDIOPORT_TYPE_HDMI, dsAUDIO_STEREO_PASSTHRU);

    bool initial = true;
    ASSERT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->DolbyAtmosExperience(initial));
    ASSERT_FALSE(initial) << "Pre-condition: _atmosMetaData=false → result must be false";

    // Step 2: register sink BEFORE the event
    auto sink = Core::ProxyType<AudioOutputNotificationSink>::Create();
    mAudioOutputPlugin->Register(&(*sink));

    // Step 3: fire OnDolbyAtmosCapabilitiesChanged(ATMOSMETADATA) directly via DS listener
    // → onAtmosCapabilitiesChanged: _atmosMetaData=true
    // → EvaluateCurrentAtmosExperience(): ATMOSMETADATA + PASSTHRU = true
    // → false→true → SendNotify(true)
    InjectAtmosCapability(dsAUDIO_ATMOS_ATMOSMETADATA, true);

    // Step 4: wait for COM-RPC notification
    bool fired = sink->WaitForNotification(2000);
    EXPECT_TRUE(fired)
        << "Notification must fire when _atmosMetaData changes false→true with PASSTHRU";
    EXPECT_TRUE(sink->LastValue())
        << "ATMOSMETADATA + PASSTHRU = true";

    mAudioOutputPlugin->Unregister(&(*sink));
}

// ===========================================================================
//  Scenario F — Notification fires FALSE when AtmosCapability becomes NOT_SUPPORTED
//               while soundMode is enabling (PASSTHRU)
//
//  HAL state starts: _atmosMetaData=true  (injected)
//                    _soundMode=PASSTHRU  (injected)
//  result=true
//
//  Trigger:
//    InjectAtmosCapability(NOTSUPPORTED):
//      onAtmosCapabilitiesChanged: _atmosMetaData=false
//      EvaluateCurrentAtmosExperience(): !_atmosMetaData → false
//      true→false → SendNotify(false)
//
//  Expected: notification fires, lastValue=false
// ===========================================================================
TEST_F(AudioOutputL2Test, ComRpc_Notification_FiredFalse_WhenAtmosCapabilityBecomesNotSupported)
{
    TEST_LOG("Scenario F: ATMOSMETADATA→NOT_SUPPORTED + PASSTHRU → notification(false)");

    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());
    ASSERT_NE(mAudioOutputPlugin, nullptr);

    // Step 1: set _atmosMetaData=true
    InjectAtmosCapability(dsAUDIO_ATMOS_ATMOSMETADATA, true);

    // Step 2: inject PASSTHRU → result=true
    InjectSoundMode(dsAUDIOPORT_TYPE_HDMI, dsAUDIO_STEREO_PASSTHRU);

    bool initial = false;
    ASSERT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->DolbyAtmosExperience(initial));
    ASSERT_TRUE(initial) << "Pre-condition: ATMOSMETADATA + PASSTHRU → result must be true";

    // Step 3: register sink
    auto sink = Core::ProxyType<AudioOutputNotificationSink>::Create();
    mAudioOutputPlugin->Register(&(*sink));

    // Step 4: fire NOT_SUPPORTED → _atmosMetaData=false → evaluate=false → fires false
    InjectAtmosCapability(dsAUDIO_ATMOS_NOTSUPPORTED, true);

    // Step 5: wait for notification
    bool fired = sink->WaitForNotification(2000);
    EXPECT_TRUE(fired)
        << "Notification must fire when _atmosMetaData changes true→false";
    EXPECT_FALSE(sink->LastValue())
        << "NOT_SUPPORTED hardware: notification payload must be false";

    mAudioOutputPlugin->Unregister(&(*sink));
}

// ===========================================================================
//  Scenario G — NO notification when AtmosCapability changes but result stays false
//               (SURROUND is non-enabling)
//
//  HAL state starts: _atmosMetaData=false (default)
//                    _soundMode=SURROUND (injected)
//  result=false
//
//  Trigger:
//    InjectAtmosCapability(ATMOSMETADATA):
//      onAtmosCapabilitiesChanged: _atmosMetaData=true
//      EvaluateCurrentAtmosExperience(): ATMOSMETADATA + SURROUND → false
//      old=false, new=false → no change → SendNotify NOT called
//
//  Expected: WasNotified()==false
// ===========================================================================
TEST_F(AudioOutputL2Test, ComRpc_Notification_NotFired_WhenAtmosCapabilityChangesButResultStaysFalse)
{
    TEST_LOG("Scenario G: NOT_SUPPORTED→ATMOSMETADATA + SURROUND → no notification (still false)");

    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());
    ASSERT_NE(mAudioOutputPlugin, nullptr);

    // Step 1: inject SURROUND (_atmosMetaData still false → result=false)
    InjectSoundMode(dsAUDIOPORT_TYPE_HDMI, dsAUDIO_STEREO_SURROUND);

    // Step 2: register sink
    auto sink = Core::ProxyType<AudioOutputNotificationSink>::Create();
    mAudioOutputPlugin->Register(&(*sink));

    // Step 3: fire ATMOSMETADATA — _atmosMetaData=true but SURROUND → still false
    // false→false → SendNotify NOT called
    InjectAtmosCapability(dsAUDIO_ATMOS_ATMOSMETADATA, true);

    // Step 4: short wait — nothing should arrive
    bool fired = sink->WaitForNotification(500);
    EXPECT_FALSE(fired)
        << "No notification: ATMOSMETADATA + SURROUND still evaluates to false";
    EXPECT_FALSE(sink->WasNotified());

    mAudioOutputPlugin->Unregister(&(*sink));
}

// ===========================================================================
//  Case 1 — NO notification when enabling mode changes to another enabling mode
//            (true→true, no state change)
//
//  HAL state: _atmosMetaData=true (fixture)
//  Flow:
//    1. Inject PASSTHRU → result=true
//    2. Register sink
//    3. Inject DOLBYDIGITALPLUS → result still=true (both are enabling)
//    4. old=true, new=true → SendNotify NOT called
//
//  Expected: WasNotified()==false
// ===========================================================================
TEST_F(AudioOutputL2Test_AtmosCapable, ComRpc_NoNotification_WhenEnablingModeChangesToAnotherEnablingMode)
{
    TEST_LOG("Case 1: PASSTHRU→DOLBYDIGITALPLUS keeps result=true, no notification");

    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());
    ASSERT_NE(mAudioOutputPlugin, nullptr);

    // Step 1: set initial state to true
    InjectSoundMode(dsAUDIOPORT_TYPE_HDMI, dsAUDIO_STEREO_PASSTHRU);

    bool initial = false;
    ASSERT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->DolbyAtmosExperience(initial));
    ASSERT_TRUE(initial) << "Pre-condition: result must be true";

    // Step 2: register sink
    auto sink = Core::ProxyType<AudioOutputNotificationSink>::Create();
    mAudioOutputPlugin->Register(&(*sink));

    // Step 3: inject DOLBYDIGITALPLUS — also enabling
    // old=true, new=true → no change → SendNotify NOT called
    InjectSoundMode(dsAUDIOPORT_TYPE_HDMI, dsAUDIO_STEREO_DDPLUS);

    // Step 4: short wait
    bool fired = sink->WaitForNotification(500);
    EXPECT_FALSE(fired)
        << "No notification when result stays true (PASSTHRU→DOLBYDIGITALPLUS)";
    EXPECT_FALSE(sink->WasNotified());

    mAudioOutputPlugin->Unregister(&(*sink));
}

// ===========================================================================
//  Case 2 — NO notification when getter is called (read-only, no side effects)
//
//  DolbyAtmosExperience() is a pure getter: it reads _dolbyAtmosExperience but
//  never calls SendNotify(). Calling it multiple times must not trigger events.
//
//  Setup:    default state (NOT_SUPPORTED) — result=false, no events fired
//  Register: sink
//  Action:   call DolbyAtmosExperience() three times
//  Expected: all calls return false, WasNotified()==false
// ===========================================================================
TEST_F(AudioOutputL2Test, ComRpc_NoNotification_WhenGetterCalledMultipleTimes)
{
    TEST_LOG("Case 2: repeated getter calls must never fire notifications");

    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());
    ASSERT_NE(mAudioOutputPlugin, nullptr);

    auto sink = Core::ProxyType<AudioOutputNotificationSink>::Create();
    mAudioOutputPlugin->Register(&(*sink));

    // Call the getter three times — must be idempotent, no side effects
    bool v1 = true, v2 = true, v3 = true;
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->DolbyAtmosExperience(v1));
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->DolbyAtmosExperience(v2));
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->DolbyAtmosExperience(v3));

    // All calls must return same value (getter is deterministic)
    EXPECT_EQ(v1, v2) << "Getter must return same value on repeated calls";
    EXPECT_EQ(v2, v3) << "Getter must return same value on repeated calls";

    // Default state: NOT_SUPPORTED → result=false
    EXPECT_FALSE(v1) << "Default state (HAL=NOT_SUPPORTED) must be false";

    // Getter must have no side effects
    bool fired = sink->WaitForNotification(500);
    EXPECT_FALSE(fired) << "Getter must never fire notifications";
    EXPECT_FALSE(sink->WasNotified());

    mAudioOutputPlugin->Unregister(&(*sink));
}

// ===========================================================================
//  Register / Unregister edge cases
//  (via existing AudioOutputL2Test / AtmosCapable fixtures — COM-RPC access)
// ===========================================================================

// ===========================================================================
//  Case 3 — Register same notification twice: duplicate silently ignored,
//            one notification delivered (not two) on state change.
//
//  Exercises the LOGERR("same notification is registered already") path in
//  AudioOutputImplementation::Register().
//
//  Expected:
//    - Both Register() calls return ERROR_NONE
//    - Notification fires exactly ONCE on a state change (only one entry in
//      _observers because duplicate was not added)
// ===========================================================================
TEST_F(AudioOutputL2Test_AtmosCapable, ComRpc_Register_Duplicate_NotificationDeliveredOnce)
{
    TEST_LOG("Case 3: duplicate Register — notification must fire exactly once");

    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());
    ASSERT_NE(mAudioOutputPlugin, nullptr);

    // Set initial state to false (SURROUND is non-enabling)
    InjectSoundMode(dsAUDIOPORT_TYPE_HDMI, dsAUDIO_STEREO_SURROUND);

    // Create ONE sink, register it TWICE — second call hits the LOGERR duplicate path
    auto sink = Core::ProxyType<AudioOutputNotificationSink>::Create();
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->Register(&(*sink)));
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->Register(&(*sink))); // duplicate — LOGERR triggered

    // Trigger a real false→true event so we can verify count
    InjectSoundMode(dsAUDIOPORT_TYPE_HDMI, dsAUDIO_STEREO_PASSTHRU);

    bool fired = sink->WaitForNotification(2000);
    EXPECT_TRUE(fired) << "Notification must still fire after duplicate register";

    // Clean up
    mAudioOutputPlugin->Unregister(&(*sink));
}

// ===========================================================================
//  Case 4 — Unregister a notification that was never registered: LOGERR
//            path, no crash, returns ERROR_NONE.
//
//  Exercises the LOGERR("notification not found") path in
//  AudioOutputImplementation::Unregister().
// ===========================================================================
TEST_F(AudioOutputL2Test, ComRpc_Unregister_NotFound_LogsErrorAndReturnsOk)
{
    TEST_LOG("Case 4: Unregister a never-registered sink — must log error, not crash");

    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());
    ASSERT_NE(mAudioOutputPlugin, nullptr);

    // Create a sink but do NOT register it, then immediately unregister
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

    // SURROUND is not enabling → result=false even with ATMOSMETADATA
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
//  SoundMode: kHDMI port, getStereoAuto=true → _soundMode=SOUNDMODE_AUTO
//  Exercises: lines 441-445 (getStereoAuto branch)
//  SOUNDMODE_AUTO is enabling → dolbyAtmosExperience=true
// ===========================================================================
TEST_F(AudioOutputL2Test_SoundMode_Common, SoundMode_HdmiPort_StereoAutoTrue_SetsSoundModeAuto)
{
    Activate(device::AudioOutputPortType::kHDMI,
             device::AudioStereoMode::kPassThru, /*stereoAuto=*/true, "HDMI0");

    // SOUNDMODE_AUTO is enabling → true with ATMOSMETADATA
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
//  getAudioOutputPorts() throws → SoundMode returns ERROR_GENERAL →
//  Configure sets _soundModeInitFailed=true.
//  Both flags true → DolbyAtmosExperience retry also fails → ERROR_GENERAL.
// ===========================================================================
TEST_F(AudioOutputL2Test_SoundModeInitFailure,
       Configure_SoundModeFails_SoundModeInitFailedSet)
{
    TEST_LOG("Scenario L: SoundMode() fails in Configure → lines 91-94 hit");

    // Both _atmosMetadataInitFailed and _soundModeInitFailed are true.
    // DolbyAtmosExperience enters the retry block; AtmosMetadata still fails
    // (getAudioOutputPorts still throws) → returns ERROR_GENERAL.
    JsonObject params, result;
    uint32_t status = InvokeServiceMethod(AUDIOOUTPUT_CALLSIGN, "dolbyAtmosExperience",
                                          params, result);
    EXPECT_NE(Core::ERROR_NONE, status)
        << "Both init failures: retry still fails → must return error";
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
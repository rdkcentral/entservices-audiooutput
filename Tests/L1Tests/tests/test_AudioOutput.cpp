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

#include <chrono>
#include <condition_variable>
#include <mutex>

#include "AudioOutputImplementation.h"

// Thunder mocks (ServiceMock, COMLinkMock)
#include "ServiceMock.h"
#include "COMLinkMock.h"

// DeviceSettings mocks — correct filenames from entservices-testframework
#include "HostMock.h"                  // defines HostImplMock  (NOT HostImplMock.h)
#include "AudioOutputPortMock.h"       // defines AudioOutputPortMock
#include "AudioOutputPortTypeMock.h"   // defines AudioOutputPortTypeMock
#include "ManagerMock.h"               // defines ManagerImplMock
#include "HdmiCecSinkMock.h"           // defines HdmiCecSinkMock

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
using ::testing::Invoke;
using ::testing::Eq;
using namespace WPEFramework;

// ---------------------------------------------------------------------------
// Global mock pointers — wired into the devicesettings.h stubs via setImpl().
// ---------------------------------------------------------------------------
HostImplMock*            p_hostImplMock            = nullptr;
AudioOutputPortMock*     p_audioOutputPortMock     = nullptr;
AudioOutputPortTypeMock* p_audioOutputPortTypeMock = nullptr;
ManagerImplMock*         p_managerImplMock         = nullptr;

// ---------------------------------------------------------------------------
// Worker pool — the plugin dispatches notifications through Core::IWorkerPool.
// ---------------------------------------------------------------------------
class TestWorkerPool : public Core::WorkerPool {
public:
    TestWorkerPool(const uint8_t threads, const uint32_t stackSize, const uint32_t queueSize)
        : Core::WorkerPool(threads, stackSize, queueSize, &_dispatcher)
    {
    }
    ~TestWorkerPool() override { Stop(); }

    void Stop() { Core::WorkerPool::Stop(); }
    void Run() { Core::WorkerPool::Run(); }

private:
    class Dispatcher : public Core::ThreadPool::IDispatcher {
    public:
        ~Dispatcher() override = default;
        void Initialize() override {}
        void Deinitialize() override {}
        void Dispatch(Core::IDispatch* job) override { job->Dispatch(); }
    } _dispatcher;
};

// Counts asynchronous notifications so tests can wait for them.
class NotificationSignal {
public:
    void Notify()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        ++_count;
        _cv.notify_all();
    }
    bool WaitFor(int count, int timeoutMs = 2000)
    {
        std::unique_lock<std::mutex> lock(_mutex);
        return _cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return _count >= count; });
    }

private:
    std::mutex _mutex;
    std::condition_variable _cv;
    int _count{0};
};

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
    NiceMock<HdmiCecSinkMock>         cecMock;

    // Listeners the plugin registers; captured so tests can fire the DS / CEC events directly.
    device::Host::IAudioOutputPortEvents*          audioListener{nullptr};
    device::Host::IHdmiInEvents*                   hdmiListener{nullptr};
    Exchange::IHdmiCecSink::INotification*         cecNotification{nullptr};

    device::AudioOutputPortType portTypeObj;
    device::AudioOutputPort     portObj;

    // Port name strings for getName() ReturnRef — must outlive mock calls.
    std::string portName{"HDMI0"};
    std::string arcPortName{"HDMI_ARC0"};

    Core::ProxyType<TestWorkerPool> workerPool;
    Core::ProxyType<Plugin::AudioOutputImplementation> impl;

    void SetUp() override
    {
        p_hostImplMock            = &hostImplMock;
        p_audioOutputPortMock     = &audioOutputPortMock;
        p_audioOutputPortTypeMock = &audioOutputPortTypeMock;
        p_managerImplMock         = &managerImplMock;

        device::Host::setImpl(p_hostImplMock);
        device::AudioOutputPort::setImpl(p_audioOutputPortMock);
        device::AudioOutputPortType::setImpl(p_audioOutputPortTypeMock);
        device::Manager::setImpl(p_managerImplMock);

        ON_CALL(managerImplMock, Initialize()).WillByDefault(Return());
        ON_CALL(managerImplMock, DeInitialize()).WillByDefault(Return());

        // No audio ports by default; getAudioOutputPort() always yields portObj.
        ON_CALL(hostImplMock, getAudioOutputPorts())
            .WillByDefault(Return(device::List<device::AudioOutputPort>{}));
        ON_CALL(hostImplMock, getAudioOutputPort(_)).WillByDefault(ReturnRef(portObj));

        ON_CALL(hostImplMock, Register(testing::A<device::Host::IAudioOutputPortEvents*>()))
            .WillByDefault(DoAll(SaveArg<0>(&audioListener), Return(dsERR_NONE)));
        ON_CALL(hostImplMock, Register(testing::A<device::Host::IHdmiInEvents*>()))
            .WillByDefault(DoAll(SaveArg<0>(&hdmiListener), Return(dsERR_NONE)));
        ON_CALL(cecMock, Register(_))
            .WillByDefault(DoAll(SaveArg<0>(&cecNotification), Return(Core::ERROR_NONE)));

        workerPool = Core::ProxyType<TestWorkerPool>::Create(2, Core::Thread::DefaultStackSize(), 16);
        Core::IWorkerPool::Assign(&(*workerPool));
        workerPool->Run();

        impl = Core::ProxyType<Plugin::AudioOutputImplementation>::Create();

        ASSERT_NE(audioListener, nullptr) << "Plugin did not register for IAudioOutputPortEvents";
        ASSERT_NE(hdmiListener, nullptr) << "Plugin did not register for IHdmiInEvents";
    }

    void TearDown() override
    {
        impl = Core::ProxyType<Plugin::AudioOutputImplementation>();

        workerPool->Stop();
        Core::IWorkerPool::Assign(nullptr);
        workerPool.Release();

        device::Host::setImpl(nullptr);
        device::AudioOutputPort::setImpl(nullptr);
        device::AudioOutputPortType::setImpl(nullptr);
        device::Manager::setImpl(nullptr);

        p_hostImplMock            = nullptr;
        p_audioOutputPortMock     = nullptr;
        p_audioOutputPortTypeMock = nullptr;
        p_managerImplMock         = nullptr;
    }

    // HAL reports the given Atmos capability for the connected sink (STB path: HDMI0).
    void SetAtmosCapability(dsATMOSCapability_t cap)
    {
        ON_CALL(audioOutputPortMock, isConnected()).WillByDefault(Return(true));
        ON_CALL(audioOutputPortMock, getSinkDeviceAtmosCapability(_))
            .WillByDefault(DoAll(SetArgReferee<0>(cap), Return(true)));
    }

    // HAL exposes one enabled+connected port of the given type with the given stereo mode.
    void SetSoundMode(int portType, const std::string& name, dsAudioStereoMode_t smode, bool stereoAuto = false)
    {
        ON_CALL(hostImplMock, getAudioOutputPorts())
            .WillByDefault(Return(device::List<device::AudioOutputPort>{device::AudioOutputPort()}));
        ON_CALL(audioOutputPortMock, isEnabled()).WillByDefault(Return(true));
        ON_CALL(audioOutputPortMock, isConnected()).WillByDefault(Return(true));
        ON_CALL(audioOutputPortMock, getType()).WillByDefault(ReturnRef(portTypeObj));
        ON_CALL(audioOutputPortTypeMock, getId()).WillByDefault(Return(portType));
        ON_CALL(audioOutputPortMock, getName()).WillByDefault(ReturnRef(name));
        ON_CALL(audioOutputPortMock, getStereoMode())
            .WillByDefault(Return(device::AudioStereoMode(smode)));
        ON_CALL(audioOutputPortMock, getStereoAuto()).WillByDefault(Return(stereoAuto));
    }

    bool Query(Core::hresult expected = Core::ERROR_NONE)
    {
        bool enabled = false;
        EXPECT_EQ(expected, impl->DolbyAtmosExperience(enabled));
        return enabled;
    }

    // DS HAL events — dispatched to the worker pool by the plugin.
    void FireAtmosCapability(dsATMOSCapability_t cap)
    {
        audioListener->OnDolbyAtmosCapabilitiesChanged(cap, true);
    }

    void FireAudioMode(int portType, dsAudioStereoMode_t smode)
    {
        audioListener->OnAudioModeEvent(static_cast<dsAudioPortType_t>(portType), smode);
    }

    void FireHdmiHotPlug(bool connected)
    {
        hdmiListener->OnHdmiInEventHotPlug(dsHDMI_IN_PORT_0, connected);
    }

    // Configure() with HdmiCecSink available; fills cecNotification.
    void ConfigureWithCec()
    {
        NiceMock<ServiceMock> serviceMock;
        ON_CALL(serviceMock, QueryInterfaceByCallsign(static_cast<uint32_t>(Exchange::IHdmiCecSink::ID), _))
            .WillByDefault(Return(static_cast<void*>(static_cast<Exchange::IHdmiCecSink*>(&cecMock))));
        EXPECT_EQ(Core::ERROR_NONE, impl->Configure(&serviceMock));
        ASSERT_NE(cecNotification, nullptr) << "Plugin did not register for HdmiCecSink notifications";
    }

    // Registers an observer expecting exactly one OnDolbyAtmosExperienceChanged(value).
    Core::ProxyType<MockAudioOutputNotification> ExpectNotification(NotificationSignal& signal, bool value)
    {
        auto observer = Core::ProxyType<MockAudioOutputNotification>::Create();
        impl->Register(&(*observer));
        EXPECT_CALL(*observer, OnDolbyAtmosExperienceChanged(value))
            .Times(1)
            .WillOnce(Invoke([&](bool) { signal.Notify(); }));
        return observer;
    }

    // Registers an observer that must never be notified.
    Core::ProxyType<MockAudioOutputNotification> ExpectNoNotification()
    {
        auto observer = Core::ProxyType<MockAudioOutputNotification>::Create();
        impl->Register(&(*observer));
        EXPECT_CALL(*observer, OnDolbyAtmosExperienceChanged(_)).Times(0);
        return observer;
    }

    // Gives an unexpected asynchronous notification time to surface.
    static void Settle()
    {
        NotificationSignal none;
        none.WaitFor(1, 200);
    }
};

// ===========================================================================
// DolbyAtmosExperience — evaluation (every call queries the HAL, no cache)
// ===========================================================================

TEST_F(AudioOutputImplementationTest, DolbyAtmosExperience_NoPorts_ReturnsFalse)
{
    bool enabled = true;
    EXPECT_EQ(Core::ERROR_NONE, impl->DolbyAtmosExperience(enabled));
    EXPECT_FALSE(enabled);
}

TEST_F(AudioOutputImplementationTest, NotSupportedCapability_AllSoundModes_ReturnFalse)
{
    SetAtmosCapability(dsAUDIO_ATMOS_NOTSUPPORTED);

    const dsAudioStereoMode_t modes[] = {
        dsAUDIO_STEREO_MONO, dsAUDIO_STEREO_STEREO, dsAUDIO_STEREO_SURROUND,
        dsAUDIO_STEREO_PASSTHRU, dsAUDIO_STEREO_DDPLUS,
    };
    for (auto mode : modes) {
        SetSoundMode(device::AudioOutputPortType::kHDMI, portName, mode);
        EXPECT_FALSE(Query()) << "mode=" << static_cast<int>(mode);
    }
}

TEST_F(AudioOutputImplementationTest, AtmosMetadata_EnablingSoundModes_ReturnTrue)
{
    SetAtmosCapability(dsAUDIO_ATMOS_ATMOSMETADATA);

    const dsAudioStereoMode_t modes[] = {
        dsAUDIO_STEREO_PASSTHRU, dsAUDIO_STEREO_DDPLUS, dsAUDIO_STEREO_SURROUND,
    };
    for (auto mode : modes) {
        SetSoundMode(device::AudioOutputPortType::kHDMI, portName, mode);
        EXPECT_TRUE(Query()) << "mode=" << static_cast<int>(mode);
    }
}

TEST_F(AudioOutputImplementationTest, AtmosMetadata_ArcStereoAuto_ReturnsTrue)
{
    SetAtmosCapability(dsAUDIO_ATMOS_ATMOSMETADATA);
    // kARC + stereoAuto → SOUNDMODE_AUTO regardless of the reported stereo mode
    SetSoundMode(device::AudioOutputPortType::kARC, arcPortName, dsAUDIO_STEREO_STEREO, true);
    EXPECT_TRUE(Query());
}

TEST_F(AudioOutputImplementationTest, AtmosMetadata_NonEnablingSoundModes_ReturnFalse)
{
    SetAtmosCapability(dsAUDIO_ATMOS_ATMOSMETADATA);

    const dsAudioStereoMode_t modes[] = {
        dsAUDIO_STEREO_MONO, dsAUDIO_STEREO_STEREO, dsAUDIO_STEREO_DD, dsAUDIO_STEREO_UNKNOWN,
    };
    for (auto mode : modes) {
        SetSoundMode(device::AudioOutputPortType::kHDMI, portName, mode);
        EXPECT_FALSE(Query()) << "mode=" << static_cast<int>(mode);
    }
}

TEST_F(AudioOutputImplementationTest, AtmosMetadata_Hdmi0NotConnected_UsesHostCapability)
{
    // Host-level getter takes its argument by value, so it cannot report ATMOSMETADATA here
    EXPECT_CALL(hostImplMock, getSinkDeviceAtmosCapability(_)).Times(1);
    // Port is listed and enabled but not connected → no sound mode port selected
    SetSoundMode(device::AudioOutputPortType::kHDMI, portName, dsAUDIO_STEREO_PASSTHRU);
    ON_CALL(audioOutputPortMock, isConnected()).WillByDefault(Return(false));

    EXPECT_FALSE(Query());
}

// ===========================================================================
// SoundMode — port-type branches and precedence
// ===========================================================================

TEST_F(AudioOutputImplementationTest, SoundMode_SpeakerPort_Surround_ReturnsTrue)
{
    SetAtmosCapability(dsAUDIO_ATMOS_ATMOSMETADATA);
    SetSoundMode(device::AudioOutputPortType::kSPEAKER, portName, dsAUDIO_STEREO_SURROUND);
    EXPECT_TRUE(Query());
}

TEST_F(AudioOutputImplementationTest, SoundMode_SpdifPort_Passthru_ReturnsTrue)
{
    SetAtmosCapability(dsAUDIO_ATMOS_ATMOSMETADATA);
    SetSoundMode(device::AudioOutputPortType::kSPDIF, portName, dsAUDIO_STEREO_PASSTHRU);
    EXPECT_TRUE(Query());
}

TEST_F(AudioOutputImplementationTest, SoundMode_HeadphonePort_Mono_ReturnsFalse)
{
    SetAtmosCapability(dsAUDIO_ATMOS_ATMOSMETADATA);
    SetSoundMode(device::AudioOutputPortType::kHEADPHONE, portName, dsAUDIO_STEREO_MONO);
    EXPECT_FALSE(Query());
}

TEST_F(AudioOutputImplementationTest, SoundMode_ArcPort_DolbyDigitalPlus_ReturnsTrue)
{
    SetAtmosCapability(dsAUDIO_ATMOS_ATMOSMETADATA);
    SetSoundMode(device::AudioOutputPortType::kARC, arcPortName, dsAUDIO_STEREO_DDPLUS);
    EXPECT_TRUE(Query());
}

// isConnected(): AtmosMetadata → true, SoundMode loop → true, SoundMode inner check → false.
TEST_F(AudioOutputImplementationTest, SoundMode_SelectedPortNoLongerConnected_ReturnsFalse)
{
    SetSoundMode(device::AudioOutputPortType::kHDMI, portName, dsAUDIO_STEREO_PASSTHRU);
    ON_CALL(audioOutputPortMock, getSinkDeviceAtmosCapability(_))
        .WillByDefault(DoAll(SetArgReferee<0>(dsAUDIO_ATMOS_ATMOSMETADATA), Return(true)));
    EXPECT_CALL(audioOutputPortMock, isConnected())
        .WillOnce(Return(true))
        .WillOnce(Return(true))
        .WillRepeatedly(Return(false));

    EXPECT_FALSE(Query());
}

// ===========================================================================
// Error handling — HAL exceptions
// ===========================================================================

TEST_F(AudioOutputImplementationTest, AtmosMetadata_HalThrows_ReturnsErrorGeneral)
{
    ON_CALL(hostImplMock, getAudioOutputPort(_))
        .WillByDefault(Throw(device::Exception("getAudioOutputPort failed")));
    Query(Core::ERROR_GENERAL);
}

TEST_F(AudioOutputImplementationTest, SoundMode_HalThrows_ReturnsErrorGeneral)
{
    SetSoundMode(device::AudioOutputPortType::kHDMI, portName, dsAUDIO_STEREO_PASSTHRU);
    ON_CALL(audioOutputPortMock, isEnabled())
        .WillByDefault(Throw(device::Exception("isEnabled failed")));
    Query(Core::ERROR_GENERAL);
}

TEST_F(AudioOutputImplementationTest, DolbyAtmosExperience_RecoversAfterHalFailure)
{
    ON_CALL(hostImplMock, getAudioOutputPort(_))
        .WillByDefault(Throw(device::Exception("getAudioOutputPort failed")));
    Query(Core::ERROR_GENERAL);

    ON_CALL(hostImplMock, getAudioOutputPort(_)).WillByDefault(ReturnRef(portObj));
    SetAtmosCapability(dsAUDIO_ATMOS_ATMOSMETADATA);
    SetSoundMode(device::AudioOutputPortType::kHDMI, portName, dsAUDIO_STEREO_PASSTHRU);
    EXPECT_TRUE(Query());
}

// ===========================================================================
// Notifications — DolbyAtmosExperience() is a pure getter; notifications come from
// DS audio events, HDMI-In hot plug and CEC power status, and fire only on change.
// ===========================================================================

TEST_F(AudioOutputImplementationTest, Getter_NeverNotifies)
{
    auto observer = ExpectNoNotification();

    SetAtmosCapability(dsAUDIO_ATMOS_ATMOSMETADATA);
    SetSoundMode(device::AudioOutputPortType::kHDMI, portName, dsAUDIO_STEREO_PASSTHRU);
    EXPECT_TRUE(Query());
    EXPECT_TRUE(Query());

    Settle();
    impl->Unregister(&(*observer));
}

TEST_F(AudioOutputImplementationTest, AudioModeEvent_FalseToTrue_Notifies)
{
    NotificationSignal signal;
    auto observer = ExpectNotification(signal, true);

    SetAtmosCapability(dsAUDIO_ATMOS_ATMOSMETADATA);
    SetSoundMode(device::AudioOutputPortType::kHDMI, portName, dsAUDIO_STEREO_PASSTHRU);
    FireAudioMode(device::AudioOutputPortType::kHDMI, dsAUDIO_STEREO_PASSTHRU);

    EXPECT_TRUE(signal.WaitFor(1));
    impl->Unregister(&(*observer));
}

TEST_F(AudioOutputImplementationTest, AudioModeEvent_TrueToFalse_Notifies)
{
    NotificationSignal signal;
    auto observer = Core::ProxyType<MockAudioOutputNotification>::Create();
    impl->Register(&(*observer));
    EXPECT_CALL(*observer, OnDolbyAtmosExperienceChanged(true))
        .WillOnce(Invoke([&](bool) { signal.Notify(); }));
    EXPECT_CALL(*observer, OnDolbyAtmosExperienceChanged(false))
        .WillOnce(Invoke([&](bool) { signal.Notify(); }));

    SetAtmosCapability(dsAUDIO_ATMOS_ATMOSMETADATA);
    SetSoundMode(device::AudioOutputPortType::kHDMI, portName, dsAUDIO_STEREO_PASSTHRU);
    FireAudioMode(device::AudioOutputPortType::kHDMI, dsAUDIO_STEREO_PASSTHRU);
    EXPECT_TRUE(signal.WaitFor(1));

    SetSoundMode(device::AudioOutputPortType::kHDMI, portName, dsAUDIO_STEREO_STEREO);
    FireAudioMode(device::AudioOutputPortType::kHDMI, dsAUDIO_STEREO_STEREO);
    EXPECT_TRUE(signal.WaitFor(2));

    impl->Unregister(&(*observer));
}

TEST_F(AudioOutputImplementationTest, AudioModeEvent_ValueUnchanged_NotifiesOnce)
{
    NotificationSignal signal;
    auto observer = ExpectNotification(signal, true);

    SetAtmosCapability(dsAUDIO_ATMOS_ATMOSMETADATA);
    SetSoundMode(device::AudioOutputPortType::kHDMI, portName, dsAUDIO_STEREO_PASSTHRU);
    FireAudioMode(device::AudioOutputPortType::kHDMI, dsAUDIO_STEREO_PASSTHRU);
    EXPECT_TRUE(signal.WaitFor(1));

    // PASSTHRU -> DDPLUS keeps the result true
    SetSoundMode(device::AudioOutputPortType::kHDMI, portName, dsAUDIO_STEREO_DDPLUS);
    FireAudioMode(device::AudioOutputPortType::kHDMI, dsAUDIO_STEREO_DDPLUS);

    Settle();
    impl->Unregister(&(*observer));
}

// Event for ARC while only the speaker is connected must be ignored.
TEST_F(AudioOutputImplementationTest, AudioModeEvent_PortTypeNotConnected_Ignored)
{
    auto observer = ExpectNoNotification();

    SetAtmosCapability(dsAUDIO_ATMOS_ATMOSMETADATA);
    SetSoundMode(device::AudioOutputPortType::kSPEAKER, portName, dsAUDIO_STEREO_PASSTHRU);
    FireAudioMode(device::AudioOutputPortType::kARC, dsAUDIO_STEREO_DDPLUS);

    Settle();
    impl->Unregister(&(*observer));
}

TEST_F(AudioOutputImplementationTest, AudioModeEvent_GetPortsThrows_Ignored)
{
    auto observer = ExpectNoNotification();

    SetAtmosCapability(dsAUDIO_ATMOS_ATMOSMETADATA);
    ON_CALL(hostImplMock, getAudioOutputPorts())
        .WillByDefault(Throw(device::Exception("getAudioOutputPorts failed")));
    FireAudioMode(device::AudioOutputPortType::kHDMI, dsAUDIO_STEREO_PASSTHRU);

    Settle();
    impl->Unregister(&(*observer));
}

TEST_F(AudioOutputImplementationTest, AudioModeEvent_AtmosMetadataFails_Ignored)
{
    auto observer = ExpectNoNotification();

    SetSoundMode(device::AudioOutputPortType::kHDMI, portName, dsAUDIO_STEREO_PASSTHRU);
    ON_CALL(hostImplMock, getAudioOutputPort(_))
        .WillByDefault(Throw(device::Exception("getAudioOutputPort failed")));
    FireAudioMode(device::AudioOutputPortType::kHDMI, dsAUDIO_STEREO_PASSTHRU);

    Settle();
    impl->Unregister(&(*observer));
}

TEST_F(AudioOutputImplementationTest, AtmosCapabilityEvent_Supported_Notifies)
{
    NotificationSignal signal;
    auto observer = ExpectNotification(signal, true);

    SetSoundMode(device::AudioOutputPortType::kHDMI, portName, dsAUDIO_STEREO_PASSTHRU);
    FireAtmosCapability(dsAUDIO_ATMOS_ATMOSMETADATA);

    EXPECT_TRUE(signal.WaitFor(1));
    impl->Unregister(&(*observer));
}

TEST_F(AudioOutputImplementationTest, AtmosCapabilityEvent_NotSupported_NotifiesFalse)
{
    NotificationSignal signal;
    auto observer = Core::ProxyType<MockAudioOutputNotification>::Create();
    impl->Register(&(*observer));
    EXPECT_CALL(*observer, OnDolbyAtmosExperienceChanged(true))
        .WillOnce(Invoke([&](bool) { signal.Notify(); }));
    EXPECT_CALL(*observer, OnDolbyAtmosExperienceChanged(false))
        .WillOnce(Invoke([&](bool) { signal.Notify(); }));

    SetSoundMode(device::AudioOutputPortType::kHDMI, portName, dsAUDIO_STEREO_PASSTHRU);
    FireAtmosCapability(dsAUDIO_ATMOS_ATMOSMETADATA);
    EXPECT_TRUE(signal.WaitFor(1));

    FireAtmosCapability(dsAUDIO_ATMOS_NOTSUPPORTED);
    EXPECT_TRUE(signal.WaitFor(2));

    impl->Unregister(&(*observer));
}

TEST_F(AudioOutputImplementationTest, AtmosCapabilityEvent_ResultStaysFalse_NoNotification)
{
    auto observer = ExpectNoNotification();

    SetSoundMode(device::AudioOutputPortType::kHDMI, portName, dsAUDIO_STEREO_STEREO);
    FireAtmosCapability(dsAUDIO_ATMOS_ATMOSMETADATA);

    Settle();
    impl->Unregister(&(*observer));
}

TEST_F(AudioOutputImplementationTest, AtmosCapabilityEvent_SoundModeFails_Ignored)
{
    auto observer = ExpectNoNotification();

    ON_CALL(hostImplMock, getAudioOutputPorts())
        .WillByDefault(Throw(device::Exception("getAudioOutputPorts failed")));
    FireAtmosCapability(dsAUDIO_ATMOS_ATMOSMETADATA);

    Settle();
    impl->Unregister(&(*observer));
}

TEST_F(AudioOutputImplementationTest, HdmiHotPlug_ReevaluatesAndNotifies)
{
    NotificationSignal signal;
    auto observer = ExpectNotification(signal, true);

    SetAtmosCapability(dsAUDIO_ATMOS_ATMOSMETADATA);
    SetSoundMode(device::AudioOutputPortType::kARC, arcPortName, dsAUDIO_STEREO_DDPLUS);
    FireHdmiHotPlug(true);

    EXPECT_TRUE(signal.WaitFor(1));
    impl->Unregister(&(*observer));
}

TEST_F(AudioOutputImplementationTest, HdmiHotPlug_ValueUnchanged_NoNotification)
{
    auto observer = ExpectNoNotification();

    SetAtmosCapability(dsAUDIO_ATMOS_NOTSUPPORTED);
    SetSoundMode(device::AudioOutputPortType::kHDMI, portName, dsAUDIO_STEREO_PASSTHRU);
    FireHdmiHotPlug(false);

    Settle();
    impl->Unregister(&(*observer));
}

TEST_F(AudioOutputImplementationTest, HdmiHotPlug_HalFails_Ignored)
{
    auto observer = ExpectNoNotification();

    ON_CALL(hostImplMock, getAudioOutputPort(_))
        .WillByDefault(Throw(device::Exception("getAudioOutputPort failed")));
    FireHdmiHotPlug(true);

    Settle();
    impl->Unregister(&(*observer));
}

TEST_F(AudioOutputImplementationTest, CecPowerStatus_FirstReport_ReevaluatesAndNotifies)
{
    ConfigureWithCec();
    NotificationSignal signal;
    auto observer = ExpectNotification(signal, true);

    SetAtmosCapability(dsAUDIO_ATMOS_ATMOSMETADATA);
    SetSoundMode(device::AudioOutputPortType::kARC, arcPortName, dsAUDIO_STEREO_DDPLUS);
    cecNotification->ReportAudioDevicePowerStatus(0);

    EXPECT_TRUE(signal.WaitFor(1));
    impl->Unregister(&(*observer));
}

// Only the first report after boot is acted on; this event arrives frequently.
TEST_F(AudioOutputImplementationTest, CecPowerStatus_LaterReports_Ignored)
{
    ConfigureWithCec();
    NotificationSignal signal;
    auto observer = ExpectNotification(signal, true);

    SetAtmosCapability(dsAUDIO_ATMOS_ATMOSMETADATA);
    SetSoundMode(device::AudioOutputPortType::kARC, arcPortName, dsAUDIO_STEREO_DDPLUS);
    cecNotification->ReportAudioDevicePowerStatus(0);
    EXPECT_TRUE(signal.WaitFor(1));

    // Would notify false if the second report were evaluated.
    SetSoundMode(device::AudioOutputPortType::kARC, arcPortName, dsAUDIO_STEREO_STEREO);
    cecNotification->ReportAudioDevicePowerStatus(0);

    Settle();
    impl->Unregister(&(*observer));
}

TEST_F(AudioOutputImplementationTest, CecPowerStatus_HalFails_Ignored)
{
    ConfigureWithCec();
    auto observer = ExpectNoNotification();

    ON_CALL(hostImplMock, getAudioOutputPort(_))
        .WillByDefault(Throw(device::Exception("getAudioOutputPort failed")));
    cecNotification->ReportAudioDevicePowerStatus(0);

    Settle();
    impl->Unregister(&(*observer));
}

TEST_F(AudioOutputImplementationTest, Notification_NotFired_AfterUnregister)
{
    auto observer = ExpectNoNotification();
    impl->Unregister(&(*observer));

    SetAtmosCapability(dsAUDIO_ATMOS_ATMOSMETADATA);
    SetSoundMode(device::AudioOutputPortType::kHDMI, portName, dsAUDIO_STEREO_PASSTHRU);
    FireAudioMode(device::AudioOutputPortType::kHDMI, dsAUDIO_STEREO_PASSTHRU);

    Settle();
}

TEST_F(AudioOutputImplementationTest, DsStubCallbacks_NoCrash)
{
    audioListener->OnAudioOutHotPlug(dsAUDIOPORT_TYPE_HDMI, 0, true);
    audioListener->OnAudioFormatUpdate(dsAUDIO_FORMAT_PCM);
    audioListener->OnAudioPortStateChanged(dsAUDIOPORT_STATE_INITIALIZED);
    audioListener->OnAssociatedAudioMixingChanged(true);
    audioListener->OnAudioFaderControlChanged(50);
    audioListener->OnAudioPrimaryLanguageChanged("eng");
    audioListener->OnAudioSecondaryLanguageChanged("fra");

    EXPECT_FALSE(Query());
}

// ===========================================================================
// Register / Unregister edge cases
// ===========================================================================

TEST_F(AudioOutputImplementationTest, Register_DuplicateNotification_NotifiedOnce)
{
    NotificationSignal signal;
    auto notification = Core::ProxyType<MockAudioOutputNotification>::Create();
    EXPECT_EQ(Core::ERROR_NONE, impl->Register(&(*notification)));
    EXPECT_EQ(Core::ERROR_NONE, impl->Register(&(*notification)));
    EXPECT_CALL(*notification, OnDolbyAtmosExperienceChanged(true))
        .Times(1)
        .WillOnce(Invoke([&](bool) { signal.Notify(); }));

    SetAtmosCapability(dsAUDIO_ATMOS_ATMOSMETADATA);
    SetSoundMode(device::AudioOutputPortType::kHDMI, portName, dsAUDIO_STEREO_PASSTHRU);
    FireAudioMode(device::AudioOutputPortType::kHDMI, dsAUDIO_STEREO_PASSTHRU);

    EXPECT_TRUE(signal.WaitFor(1));
    Settle();
    impl->Unregister(&(*notification));
}

TEST_F(AudioOutputImplementationTest, Unregister_NotFound_ReturnsNone)
{
    auto notification = Core::ProxyType<MockAudioOutputNotification>::Create();
    EXPECT_EQ(Core::ERROR_NONE, impl->Unregister(&(*notification)));
    EXPECT_FALSE(Query());
}

// ===========================================================================
// Configure / INTERFACE_MAP
// ===========================================================================

TEST_F(AudioOutputImplementationTest, Configure_ReturnsNone)
{
    NiceMock<ServiceMock> serviceMock;
    EXPECT_EQ(Core::ERROR_NONE, impl->Configure(&serviceMock));
}

TEST_F(AudioOutputImplementationTest, Configure_RegistersForHdmiCecSink)
{
    EXPECT_CALL(cecMock, Register(_)).Times(1);
    ConfigureWithCec();
}

TEST_F(AudioOutputImplementationTest, Destructor_UnregistersAllListeners)
{
    ConfigureWithCec();

    EXPECT_CALL(cecMock, Unregister(_)).Times(1);
    EXPECT_CALL(hostImplMock, UnRegister(testing::A<device::Host::IAudioOutputPortEvents*>())).Times(1);
    EXPECT_CALL(hostImplMock, UnRegister(testing::A<device::Host::IHdmiInEvents*>())).Times(1);
    impl = Core::ProxyType<Plugin::AudioOutputImplementation>();
}

TEST_F(AudioOutputImplementationTest, InterfaceMap_QueryInterface_AudioOutput)
{
    auto* iface = static_cast<Exchange::IAudioOutput*>(impl->QueryInterface(Exchange::IAudioOutput::ID));
    ASSERT_NE(iface, nullptr);
    iface->Release();
}

TEST_F(AudioOutputImplementationTest, InterfaceMap_QueryInterface_Configuration)
{
    auto* iface = static_cast<Exchange::IConfiguration*>(impl->QueryInterface(Exchange::IConfiguration::ID));
    ASSERT_NE(iface, nullptr);
    iface->Release();
}

// ===========================================================================
// Constructor / destructor exception paths (standalone — no fixture)
// ===========================================================================

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

    ON_CALL(managerMock, Initialize())
        .WillByDefault(Throw(device::Exception("Manager init failed")));
    ON_CALL(managerMock, DeInitialize()).WillByDefault(Return());
    ON_CALL(hostMock, getAudioOutputPorts())
        .WillByDefault(Return(device::List<device::AudioOutputPort>{}));
    device::AudioOutputPort port;
    ON_CALL(hostMock, getAudioOutputPort(_)).WillByDefault(ReturnRef(port));

    // Initialize threw, so no DS listeners are registered.
    EXPECT_CALL(hostMock, Register(testing::A<device::Host::IAudioOutputPortEvents*>())).Times(0);
    EXPECT_CALL(hostMock, Register(testing::A<device::Host::IHdmiInEvents*>())).Times(0);

    {
        auto implLocal = Core::ProxyType<Plugin::AudioOutputImplementation>::Create();
        bool enabled = true;
        EXPECT_EQ(Core::ERROR_NONE, implLocal->DolbyAtmosExperience(enabled));
        EXPECT_FALSE(enabled);
    }

    device::Host::setImpl(nullptr);
    device::AudioOutputPort::setImpl(nullptr);
    device::AudioOutputPortType::setImpl(nullptr);
    device::Manager::setImpl(nullptr);
}

TEST(AudioOutputDestructorTests, ManagerDeInitializeThrows_CatchBlockHit)
{
    NiceMock<HostImplMock>            hostMock;
    NiceMock<AudioOutputPortMock>     portMock;
    NiceMock<AudioOutputPortTypeMock> portTypeMock;
    NiceMock<ManagerImplMock>         managerMock;

    device::Host::setImpl(&hostMock);
    device::AudioOutputPort::setImpl(&portMock);
    device::AudioOutputPortType::setImpl(&portTypeMock);
    device::Manager::setImpl(&managerMock);

    ON_CALL(managerMock, Initialize()).WillByDefault(Return());
    ON_CALL(managerMock, DeInitialize())
        .WillByDefault(Throw(device::Exception("Manager DeInit failed")));
    ON_CALL(hostMock, getAudioOutputPorts())
        .WillByDefault(Return(device::List<device::AudioOutputPort>{}));
    device::AudioOutputPort port;
    ON_CALL(hostMock, getAudioOutputPort(_)).WillByDefault(ReturnRef(port));

    // Listeners are unregistered before DeInitialize throws.
    EXPECT_CALL(hostMock, UnRegister(testing::A<device::Host::IAudioOutputPortEvents*>())).Times(1);
    EXPECT_CALL(hostMock, UnRegister(testing::A<device::Host::IHdmiInEvents*>())).Times(1);

    {
        auto implLocal = Core::ProxyType<Plugin::AudioOutputImplementation>::Create();
        bool enabled = true;
        EXPECT_EQ(Core::ERROR_NONE, implLocal->DolbyAtmosExperience(enabled));
        EXPECT_FALSE(enabled);
    }

    device::Host::setImpl(nullptr);
    device::AudioOutputPort::setImpl(nullptr);
    device::AudioOutputPortType::setImpl(nullptr);
    device::Manager::setImpl(nullptr);
}

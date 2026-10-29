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

// ===========================================================================
// JSON-RPC and COM-RPC return the default value (false) when DeviceSettings
// has not provided any Atmos capability / sound mode.
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
}

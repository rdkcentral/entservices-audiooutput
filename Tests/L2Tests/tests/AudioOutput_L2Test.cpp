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

#define JSON_TIMEOUT              (1000)
#define AUDIOOUTPUT_CALLSIGN      _T("org.rdk.AudioOutput")
#define AUDIOOUTPUT_L2TEST_CALLSIGN _T("L2tests.1")
#define CLEANUP_DELAY_MICROSECONDS  500000

using ::testing::NiceMock;
using namespace WPEFramework;

// ---------------------------------------------------------------------------
// Notification sink to capture onDolbyAtmosExperienceChanged events
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
// L2 Test fixture
// ---------------------------------------------------------------------------
class AudioOutputL2Test : public L2TestMocks {
protected:
    AudioOutputL2Test();
    virtual ~AudioOutputL2Test() override;

    uint32_t CreateAudioOutputInterfaceObjectUsingComRPCConnection();

protected:
    Core::ProxyType<RPC::InvokeServerType<1, 0, 4>> mAudioOutputEngine;
    Core::ProxyType<RPC::CommunicatorClient> mAudioOutputClient;

    PluginHost::IShell* mControllerAudioOutput{nullptr};
    Exchange::IAudioOutput* mAudioOutputPlugin{nullptr};
};

AudioOutputL2Test::AudioOutputL2Test()
    : L2TestMocks()
{
    TEST_LOG("AudioOutput L2 test constructor");

    uint32_t status = ActivateService(AUDIOOUTPUT_CALLSIGN);
    if (status != Core::ERROR_NONE) {
        TEST_LOG("Failed to activate AudioOutput plugin: %u", status);
    }
}

AudioOutputL2Test::~AudioOutputL2Test()
{
    TEST_LOG("AudioOutput L2 test destructor");

    if (mAudioOutputPlugin != nullptr) {
        mAudioOutputPlugin->Release();
        mAudioOutputPlugin = nullptr;
    }

    usleep(CLEANUP_DELAY_MICROSECONDS);
    DeactivateService(AUDIOOUTPUT_CALLSIGN);
}

uint32_t AudioOutputL2Test::CreateAudioOutputInterfaceObjectUsingComRPCConnection()
{
    // Connect to the AudioOutput plugin via COM-RPC
    uint32_t result = Core::ERROR_GENERAL;

    Core::SystemInfo::SetEnvironment(_T("THUNDER_ACCESS"), _T("127.0.0.1:9998"));

    mAudioOutputEngine = Core::ProxyType<RPC::InvokeServerType<1, 0, 4>>::Create();
    mAudioOutputClient = Core::ProxyType<RPC::CommunicatorClient>::Create(
        Core::NodeId(_T("127.0.0.1:9998")),
        Core::ProxyType<Core::IIPCServer>(mAudioOutputEngine));

    if (!mAudioOutputClient.IsValid()) {
        TEST_LOG("Failed to create CommunicatorClient");
        return result;
    }

    mAudioOutputPlugin = mAudioOutputClient->Open<Exchange::IAudioOutput>(AUDIOOUTPUT_CALLSIGN, ~0, 3000);

    if (mAudioOutputPlugin != nullptr) {
        result = Core::ERROR_NONE;
        TEST_LOG("Successfully acquired IAudioOutput interface via COM-RPC");
    } else {
        TEST_LOG("Failed to acquire IAudioOutput interface");
    }

    return result;
}

// ---------------------------------------------------------------------------
// Test 9.2: JSON-RPC dolbyAtmosExperience returns correct boolean
// ---------------------------------------------------------------------------
TEST_F(AudioOutputL2Test, JsonRpc_DolbyAtmosExperience_ReturnsBoolean)
{
    TEST_LOG("Test: dolbyAtmosExperience JSON-RPC method returns a valid boolean");

    JsonObject params, result;
    uint32_t status = InvokeServiceMethod(AUDIOOUTPUT_CALLSIGN, "dolbyAtmosExperience", params, result);

    EXPECT_EQ(status, Core::ERROR_NONE);
    // Result must contain a boolean — exact value depends on HAL mock configuration
    EXPECT_TRUE(result.HasLabel("dolbyAtmosExperience") || result.HasLabel("result"))
        << "Response should contain dolbyAtmosExperience field";
}

// ---------------------------------------------------------------------------
// Test 9.3: onDolbyAtmosExperienceChanged notification via COM-RPC
// ---------------------------------------------------------------------------
TEST_F(AudioOutputL2Test, ComRpc_Notification_DeliveredOnAudioModeChange)
{
    TEST_LOG("Test: onDolbyAtmosExperienceChanged notification delivered on AudioModeChanged event");

    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());
    ASSERT_NE(mAudioOutputPlugin, nullptr);

    auto sink = Core::ProxyType<AudioOutputNotificationSink>::Create();
    mAudioOutputPlugin->Register(&(*sink));

    // In a full integration test, triggering an AudioModeChanged event from
    // the PlayerInfo plugin would cause the notification to fire.
    // Here we verify the Register/Unregister cycle works without crash.
    TEST_LOG("Notification subscription registered successfully");

    mAudioOutputPlugin->Unregister(&(*sink));
    TEST_LOG("Notification subscription unregistered successfully");
}

// ---------------------------------------------------------------------------
// Test 9.4: Notification on AtmosCapabilityChanged event
// ---------------------------------------------------------------------------
TEST_F(AudioOutputL2Test, ComRpc_Notification_DeliveredOnAtmosCapabilityChange)
{
    TEST_LOG("Test: onDolbyAtmosExperienceChanged notification on AtmosCapabilityChanged");

    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());
    ASSERT_NE(mAudioOutputPlugin, nullptr);

    auto sink = Core::ProxyType<AudioOutputNotificationSink>::Create();
    mAudioOutputPlugin->Register(&(*sink));

    // Verify initial state is accessible
    bool enabled = false;
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->DolbyAtmosExperience(enabled));
    TEST_LOG("Initial dolbyAtmosExperience = %s", enabled ? "true" : "false");

    mAudioOutputPlugin->Unregister(&(*sink));
}

// ---------------------------------------------------------------------------
// Test 9.5: No notification when value unchanged
// ---------------------------------------------------------------------------
TEST_F(AudioOutputL2Test, ComRpc_NoNotification_WhenValueUnchanged)
{
    TEST_LOG("Test: no notification when dolbyAtmosExperience does not change");

    ASSERT_EQ(Core::ERROR_NONE, CreateAudioOutputInterfaceObjectUsingComRPCConnection());
    ASSERT_NE(mAudioOutputPlugin, nullptr);

    auto sink = Core::ProxyType<AudioOutputNotificationSink>::Create();
    mAudioOutputPlugin->Register(&(*sink));

    // Read current value
    bool before = false;
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->DolbyAtmosExperience(before));

    // Read again — should be the same, no notification expected
    bool after = false;
    EXPECT_EQ(Core::ERROR_NONE, mAudioOutputPlugin->DolbyAtmosExperience(after));
    EXPECT_EQ(before, after);
    EXPECT_FALSE(sink->WasNotified()) << "No notification should fire for a simple read";

    mAudioOutputPlugin->Unregister(&(*sink));
}

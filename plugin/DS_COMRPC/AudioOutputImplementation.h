/*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
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

#pragma once

#include "Module.h"
#include <interfaces/Ids.h>
#include <interfaces/IAudioOutput.h>
#include <interfaces/IConfiguration.h>
#include <interfaces/IDeviceSettingsAudio.h>

// COM-RPC path: DeviceSettingsInterface.h brings in DSHelper
// (which inherits PluginSmartInterfaceType<IDeviceSettings>) plus all DS
// sub-interface headers and cached handle utilities.
#include "DeviceSettingsInterface.h"

#include <com/com.h>
#include <core/core.h>
#include <atomic>
#include <list>

namespace WPEFramework {
namespace Plugin {

    class AudioOutputImplementation
        : public Exchange::IConfiguration
        , public Exchange::IAudioOutput
        , public DSHelper
    {
    public:
        AudioOutputImplementation(const AudioOutputImplementation&) = delete;
        AudioOutputImplementation& operator=(const AudioOutputImplementation&) = delete;

        AudioOutputImplementation();
        ~AudioOutputImplementation() override;

        BEGIN_INTERFACE_MAP(AudioOutputImplementation)
        INTERFACE_ENTRY(Exchange::IConfiguration)
        INTERFACE_ENTRY(Exchange::IAudioOutput)
        END_INTERFACE_MAP

        // IAudioOutput
        Core::hresult DolbyAtmosExperience(bool& enabled /* @out */) const override;
        Core::hresult Register(Exchange::IAudioOutput::INotification* notification) override;
        Core::hresult Unregister(const Exchange::IAudioOutput::INotification* notification) override;

        // IConfiguration
        uint32_t Configure(PluginHost::IShell* service) override;

    private:
        class AudioNotification : public Exchange::IDeviceSettingsAudio::INotification {
        public:
            explicit AudioNotification(AudioOutputImplementation& parent)
                : _parent(parent) {}
            ~AudioNotification() override = default;
            AudioNotification(const AudioNotification&) = delete;
            AudioNotification& operator=(const AudioNotification&) = delete;

            void OnDolbyAtmosCapabilitiesChanged(
                    Exchange::IDeviceSettingsAudio::DolbyAtmosCapability atmosCapability,
                    bool status) override
            {
                _parent.onAtmosCapabilitiesChanged(atmosCapability, status);
            }

            void OnAudioModeEvent(
                    Exchange::IDeviceSettingsAudio::AudioPortType audioPortType,
                    Exchange::IDeviceSettingsAudio::StereoMode     audioMode) override
            {
                _parent.onAudioModeChanged(audioPortType, audioMode);
            }

            BEGIN_INTERFACE_MAP(AudioNotification)
            INTERFACE_ENTRY(Exchange::IDeviceSettingsAudio::INotification)
            END_INTERFACE_MAP

        private:
            AudioOutputImplementation& _parent;
        };

    private:
        // DSHelper lifecycle callbacks
        void OnDeviceSettingsActivated() override;
        void OnDeviceSettingsDeactivated() override;

        // Thin wrapper: DSHelper::AcquireSubInterface<IDeviceSettingsAudio>()
        Exchange::IDeviceSettingsAudio* AcquireAudioInterface();

        // COM-RPC equivalents of DS_IARM private helpers
        uint32_t AtmosMetadata(bool& supported);
        uint32_t SoundMode(Exchange::IAudioOutput::AudioModes& mode);

        bool EvaluateCurrentAtmosExperience() const;
        void SendNotify(bool dolbyAtmosExperience);
        void UpdateCache();

        void onAudioModeChanged(Exchange::IDeviceSettingsAudio::AudioPortType portType,
                                Exchange::IDeviceSettingsAudio::StereoMode     smode);
        void onAtmosCapabilitiesChanged(Exchange::IDeviceSettingsAudio::DolbyAtmosCapability atmosCapability,
                                        bool status);

    private:
        mutable Core::CriticalSection _adminLock;

        mutable bool _atmosMetaData{false};
        mutable Exchange::IAudioOutput::AudioModes _soundMode{Exchange::IAudioOutput::UNKNOWN};
        bool _dolbyAtmosExperience{false};

        mutable std::atomic<bool> _atmosMetadataInitFailed{false};
        mutable std::atomic<bool> _soundModeInitFailed{false};

        std::list<Exchange::IAudioOutput::INotification*> _observers;

        // DS audio event notification sink
        Core::Sink<AudioNotification> _audioNotification;
    };

} // namespace Plugin
} // namespace WPEFramework

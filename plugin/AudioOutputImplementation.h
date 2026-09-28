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

#include <com/com.h>
#include <core/core.h>
#include <atomic>
#include <list>
#include <string>

// DSHelper: single COM-RPC link to the DeviceSettings plugin (IDeviceSettings root)
// plus all DS sub-interface headers and config-store helpers.
#include "DeviceSettingsInterface.h"

namespace WPEFramework {
namespace Plugin {

    class AudioOutputImplementation
        : public Exchange::IConfiguration
        , public Exchange::IAudioOutput
        , public DSHelper {

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
        Core::hresult GetSupportedAudioConfigs(Exchange::IAudioOutput::IAudioConfigListIterator*& audioConfigs /* @out */)  const override;
        Core::hresult GetAudioConfig(const std::string& audioConfig , bool& enabled /* @out */) const override;
        Core::hresult SetAudioConfig(const std::string& audioConfig , const bool enabled) override;
        // Exchange::IConfiguration
        uint32_t Configure(PluginHost::IShell* service) override;

    protected:
        // DSHelper lifecycle hooks — DeviceSettings (re-)activation / deactivation
        void OnDeviceSettingsActivated() override;
        void OnDeviceSettingsDeactivated() override;

    private:
        // DS Audio event delegate — receives DeviceSettings audio notifications over
        // COM-RPC and forwards them to the parent implementation.
        class DSAudioNotification : public Exchange::IDeviceSettingsAudio::INotification {
        private:
            DSAudioNotification(const DSAudioNotification&) = delete;
            DSAudioNotification& operator=(const DSAudioNotification&) = delete;

        public:
            explicit DSAudioNotification(AudioOutputImplementation& parent)
                : _parent(parent)
            {
            }
            ~DSAudioNotification() override = default;

            void OnDolbyAtmosCapabilitiesChanged(
                Exchange::IDeviceSettingsAudio::DolbyAtmosCapability atmosCapability,
                bool status) override
            {
                _parent.onAtmosCapabilitiesChanged(atmosCapability, status);
            }

            void OnAudioModeEvent(
                Exchange::IDeviceSettingsAudio::AudioPortType audioPortType,
                Exchange::IDeviceSettingsAudio::StereoMode audioMode) override
            {
                _parent.onAudioModeChanged(audioPortType, audioMode);
            }

            BEGIN_INTERFACE_MAP(DSAudioNotification)
                INTERFACE_ENTRY(Exchange::IDeviceSettingsAudio::INotification)
            END_INTERFACE_MAP

        private:
            AudioOutputImplementation& _parent;
        };

    private:
        // DeviceSettings COM-RPC query helpers
        uint32_t AtmosMetadata(bool& supported) const;
        uint32_t SoundMode(Exchange::IAudioOutput::AudioModes& mode) const;

        bool EvaluateCurrentAtmosExperience() const;

        void SendNotify(bool dolbyAtmosExperience);
        void UpdateCache();
        void onAudioModeChanged(Exchange::IDeviceSettingsAudio::AudioPortType audioPortType,
                                Exchange::IDeviceSettingsAudio::StereoMode audioMode);
        void onAtmosCapabilitiesChanged(Exchange::IDeviceSettingsAudio::DolbyAtmosCapability atmosCapability,
                                        bool status);

    private:
        mutable Core::CriticalSection _adminLock;

        // Cached values
        mutable bool _atmosMetaData{false};
        mutable Exchange::IAudioOutput::AudioModes _soundMode{Exchange::IAudioOutput::UNKNOWN};
        bool _dolbyAtmosExperience{false};

        // Init failure flags — atomic so they need no lock
        mutable std::atomic<bool> _atmosMetadataInitFailed{false};
        mutable std::atomic<bool> _soundModeInitFailed{false};

        // Observer list
        std::list<Exchange::IAudioOutput::INotification*> _observers;

        // DeviceSettings audio event delegate (COM-RPC)
        DSAudioNotification _dsAudioNotification{*this};
    };

} // namespace Plugin
} // namespace WPEFramework

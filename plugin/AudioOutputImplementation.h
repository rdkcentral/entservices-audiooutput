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
#include <interfaces/IDolby.h>
#include <interfaces/IConfiguration.h>

#include <com/com.h>
#include <core/core.h>
#include <list>

#include "host.hpp"
#include "dsAudio.h"

namespace WPEFramework {
namespace Plugin {

    class AudioOutputImplementation
        : public Exchange::IConfiguration
        , public Exchange::IAudioOutput {

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

        // Exchange::IConfiguration
        uint32_t Configure(PluginHost::IShell* service) override;

    private:
        class DsAudioPortNotification : public device::Host::IAudioOutputPortEvents {
        private:
            DsAudioPortNotification(const DsAudioPortNotification&) = delete;
            DsAudioPortNotification& operator=(const DsAudioPortNotification&) = delete;

        public:
            explicit DsAudioPortNotification(AudioOutputImplementation& parent)
                : _parent(parent)
            {
            }
            ~DsAudioPortNotification() override = default;

        public:
            void OnDolbyAtmosCapabilitiesChanged(dsATMOSCapability_t atmosCapability, bool status) override
            {
                _parent.onAtmosCapabilitiesChanged(atmosCapability, status);
            }

            // Stubs for other IAudioOutputPortEvents methods
            void OnAudioOutHotPlug(dsAudioPortType_t, uint32_t, bool) override {}
            void OnAudioFormatUpdate(dsAudioFormat_t) override {}
            void OnAudioPortStateChanged(dsAudioPortState_t) override {}
            void OnAssociatedAudioMixingChanged(bool) override {}
            void OnAudioFaderControlChanged(int) override {}
            void OnAudioPrimaryLanguageChanged(const std::string&) override {}
            void OnAudioSecondaryLanguageChanged(const std::string&) override {}
            void OnAudioModeEvent(dsAudioPortType_t, dsAudioStereoMode_t) override {}

        private:
            AudioOutputImplementation& _parent;
        };

        class PlayerInfoNotification : public Exchange::Dolby::IOutput::INotification {
        private:
            PlayerInfoNotification(const PlayerInfoNotification&) = delete;
            PlayerInfoNotification& operator=(const PlayerInfoNotification&) = delete;

        public:
            explicit PlayerInfoNotification(AudioOutputImplementation& parent)
                : _parent(parent)
            {
            }
            ~PlayerInfoNotification() override = default;

        public:
            void AudioModeChanged(const Exchange::Dolby::IOutput::SoundModes mode, const bool enabled) override
            {
                LOGINFO("AudioOutputImplementation::AudioModeChanged: mode=%d, enabled=%s",
                        static_cast<int>(mode), enabled ? "true" : "false");
                _parent.onAudioModeChanged(mode, enabled);
            }

            BEGIN_INTERFACE_MAP(PlayerInfoNotification)
            INTERFACE_ENTRY(Exchange::Dolby::IOutput::INotification)
            END_INTERFACE_MAP

        private:
            AudioOutputImplementation& _parent;
        };

    private:
        // HAL query helpers — logic copied from entservices-playerinfo PlatformImplementation.cpp
        uint32_t AtmosMetadata(bool& supported) const;
        uint32_t SoundMode(Exchange::Dolby::IOutput::SoundModes& mode) const;

        bool EvaluateCurrentAtmosExperience() const;

        void SendNotify(bool dolbyAtmosExperience);
        void InitializePlayerInfo();
        void UpdateCache();
        void registerDsEventHandlers();
        void unregisterDsEventHandlers();
        void onAudioModeChanged(const Exchange::Dolby::IOutput::SoundModes mode, const bool enabled);
        void onAtmosCapabilitiesChanged(dsATMOSCapability_t atmosCapability, bool status);

    private:
        mutable Core::CriticalSection _adminLock;

        // Cached values
        bool _atmosMetaData{false};
        Exchange::Dolby::IOutput::SoundModes _soundMode{Exchange::Dolby::IOutput::UNKNOWN};
        bool _dolbyAtmosExperience{false};

        // Observer list
        std::list<Exchange::IAudioOutput::INotification*> _observers;

        // Inter-plugin COM-RPC handles
        PluginHost::IShell* _service{};
        Exchange::Dolby::IOutput* _playerInfo{};   // org.rdk.PlayerInfo
        Core::Sink<PlayerInfoNotification> _playerInfoNotification{*this};

        // DS HAL event listener
        DsAudioPortNotification _dsAudioPortNotification{*this};
        bool _registeredDsEventHandlers{false};

        static constexpr const char* PLAYERINFO_CALLSIGN = "org.rdk.PlayerInfo";
    };

} // namespace Plugin
} // namespace WPEFramework

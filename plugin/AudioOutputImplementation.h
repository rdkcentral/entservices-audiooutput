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

#include <com/com.h>
#include <core/core.h>
#include <plugins/JSONRPC.h>
#include <list>

namespace WPEFramework {
namespace Plugin {

    class AudioOutputImplementation
        : public Exchange::IAudioOutput
        , public Exchange::Dolby::IOutput::INotification {

    public:
        AudioOutputImplementation(const AudioOutputImplementation&) = delete;
        AudioOutputImplementation& operator=(const AudioOutputImplementation&) = delete;

        AudioOutputImplementation();
        ~AudioOutputImplementation() override;

        BEGIN_INTERFACE_MAP(AudioOutputImplementation)
        INTERFACE_ENTRY(Exchange::IAudioOutput)
        INTERFACE_ENTRY(Exchange::Dolby::IOutput::INotification)
        END_INTERFACE_MAP

        // IAudioOutput
        Core::hresult DolbyAtmosExperience(bool& enabled /* @out */) const override;
        void Register(Exchange::IAudioOutput::INotification* notification) override;
        void Unregister(const Exchange::IAudioOutput::INotification* notification) override;

        // Dolby::IOutput::INotification — receives audioModeChanged from PlayerInfo
        void AudioModeChanged(const Exchange::Dolby::IOutput::SoundModes mode, const bool enabled) override;

        // Initialize / Deinitialize (called by SERVICE_REGISTRATION lifecycle)
        uint32_t Initialize(PluginHost::IShell* service);
        uint32_t Deinitialize(PluginHost::IShell* service);

    private:
        // HAL query helpers — logic copied from entservices-playerinfo PlatformImplementation.cpp
        uint32_t AtmosMetadata(bool& supported) const;
        uint32_t SoundMode(Exchange::Dolby::IOutput::SoundModes& mode) const;
        static Exchange::Dolby::IOutput::SoundModes DsAudioModeToSoundMode(const device::AudioStereoMode& smode);

        bool EvaluateCurrentAtmosExperience() const;

        void SendNotify(bool dolbyAtmosExperience);
        void InitializePlayerInfo();
        void InitializeDisplaySettings();
        void UpdateCache();
        void onAtmosCapabilityChanged(const JsonObject& parameters);

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
        WPEFramework::JSONRPC::LinkType<Core::JSON::IElement>* _displaySettingsClient{nullptr};  // org.rdk.DisplaySettings

        static constexpr const char* PLAYERINFO_CALLSIGN      = "org.rdk.PlayerInfo";
        static constexpr const char* DISPLAYSETTINGS_CALLSIGN = "org.rdk.DisplaySettings";
        static constexpr const char* DISPLAYSETTINGS_CALLSIGN_VER = "org.rdk.DisplaySettings.1";
    };

} // namespace Plugin
} // namespace WPEFramework

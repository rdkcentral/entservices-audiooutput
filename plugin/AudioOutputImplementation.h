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
#include <interfaces/IHdmiCecSink.h>

#include <com/com.h>
#include <core/core.h>
#include <atomic>
#include <list>
#include <string>

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
        class HdmiCecSinkNotification : public Exchange::IHdmiCecSink::INotification {
        private:
            HdmiCecSinkNotification(const HdmiCecSinkNotification&) = delete;
            HdmiCecSinkNotification& operator=(const HdmiCecSinkNotification&) = delete;

        public:
            explicit HdmiCecSinkNotification(AudioOutputImplementation* parent)
                : _parent(*parent)
            {
                ASSERT(parent != nullptr);
            }
            ~HdmiCecSinkNotification() override = default;

            BEGIN_INTERFACE_MAP(HdmiCecSinkNotification)
            INTERFACE_ENTRY(Exchange::IHdmiCecSink::INotification)
            END_INTERFACE_MAP

            void ReportAudioDeviceConnectedStatus(const string status, const string audioDeviceConnected) override
            {
                _parent.onReportAudioDeviceConnectedStatus(status, audioDeviceConnected);
            }

        private:
            AudioOutputImplementation& _parent;
        };

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
            void OnAudioModeEvent(dsAudioPortType_t type, dsAudioStereoMode_t smode) override
            {
                _parent.onAudioModeChanged(type, smode);
            }

        private:
            AudioOutputImplementation& _parent;
        };

    private:
        // HAL query helpers — logic copied from entservices-playerinfo PlatformImplementation.cpp
        uint32_t AtmosMetadata(bool& supported) const;
        uint32_t SoundMode(Exchange::IAudioOutput::AudioModes& mode) const;

        bool EvaluateCurrentAtmosExperience() const;

        void SendNotify(bool dolbyAtmosExperience);
        void UpdateCache();
        void registerDsEventHandlers();
        void unregisterDsEventHandlers();
        void onAudioModeChanged(dsAudioPortType_t type, dsAudioStereoMode_t smode);
        void onAtmosCapabilitiesChanged(dsATMOSCapability_t atmosCapability, bool status);
        void onReportAudioDeviceConnectedStatus(const string& status, const string& audioDeviceConnected);

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

        // DS HAL event listener
        DsAudioPortNotification _dsAudioPortNotification{*this};
        bool _registeredDsEventHandlers{false};

        // HdmiCecSink event listener
        Exchange::IHdmiCecSink* _hdmiCecSink{nullptr};
        Core::Sink<HdmiCecSinkNotification> _hdmiCecSinkNotification;
    };

} // namespace Plugin
} // namespace WPEFramework

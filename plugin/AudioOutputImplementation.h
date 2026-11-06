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
                _parent.dispatchEvent(EV_ATMOS_CAPABILITY_CHANGE,
                    ParamsType{ static_cast<int32_t>(atmosCapability), static_cast<int32_t>(status) });
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
                _parent.dispatchEvent(EV_AUDIOMODE_CHANGE,
                    ParamsType{ static_cast<int32_t>(type), static_cast<int32_t>(smode) });
            }

        private:
            AudioOutputImplementation& _parent;
        };

        // DS HDMI-In event listener — a hot plug (e.g. ARC device) triggers a re-evaluation of the Atmos experience.
        class DsHdmiInNotification : public device::Host::IHdmiInEvents {
        private:
            DsHdmiInNotification(const DsHdmiInNotification&) = delete;
            DsHdmiInNotification& operator=(const DsHdmiInNotification&) = delete;

        public:
            explicit DsHdmiInNotification(AudioOutputImplementation& parent)
                : _parent(parent)
            {
            }
            ~DsHdmiInNotification() override = default;

            void OnHdmiInEventHotPlug(dsHdmiInPort_t port, bool isConnected) override
            {
                _parent.OnHdmiInEventHotPlug(static_cast<int>(port), isConnected);
            }

        private:
            AudioOutputImplementation& _parent;
        };

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

            void ReportAudioDevicePowerStatus(const int powerStatus) override
            {
                _parent.ReportAudioDevicePowerStatus(powerStatus);
            }

        private:
            AudioOutputImplementation& _parent;
        };

    private:
        enum Event {
            EV_AUDIOMODE_CHANGE = 0,
            EV_ATMOS_CAPABILITY_CHANGE,
            EV_REEVALUATE_ATMOS,
            EV_ATMOS_EXP_CHANGED
        };

        // Event payload: (port type, stereo mode), (atmos capability, status) or (new dolbyAtmosExperience, unused);
        // unused for EV_REEVALUATE_ATMOS.
        struct ParamsType {
            int32_t first;
            int32_t second;
        };

        // Worker-pool job: carries (impl*, Event, ParamsType); calls impl->Dispatch() on a worker thread.
        class DispatchJob : public Core::IDispatch {
        protected:
            DispatchJob(AudioOutputImplementation* au, Event ev, const ParamsType& params)
                : _parent(au), _ev(ev), _params(params)
            {
                if (_parent != nullptr) {
                    _parent->AddRef();
                }
            }

        public:
            DispatchJob() = delete;
            DispatchJob(const DispatchJob&) = delete;
            DispatchJob& operator=(const DispatchJob&) = delete;
            ~DispatchJob() override
            {
                if (_parent != nullptr) {
                    _parent->Release();
                }
            }

            static Core::ProxyType<Core::IDispatch> Create(AudioOutputImplementation* au, Event ev, const ParamsType& params)
            {
                return Core::ProxyType<Core::IDispatch>(Core::ProxyType<DispatchJob>::Create(au, ev, params));
            }

            void Dispatch() override { _parent->Dispatch(_ev, _params); }

        private:
            AudioOutputImplementation* _parent;
            Event _ev;
            const ParamsType _params;
        };

    private:
        // HAL query helpers — logic copied from entservices-playerinfo PlatformImplementation.cpp
        uint32_t AtmosMetadata(bool& supported) const;
        uint32_t SoundMode(Exchange::IAudioOutput::AudioModes& mode) const;

        bool EvaluateCurrentAtmosExperience(Exchange::IAudioOutput::AudioModes mode, bool cap) const;

        void SendNotify(bool dolbyAtmosExperience);
        void NotifyIfAtmosExperienceChanged(Exchange::IAudioOutput::AudioModes mode, bool cap);
        void registerDsEventHandlers();
        void unregisterDsEventHandlers();
        void onAudioModeChanged(dsAudioPortType_t type, dsAudioStereoMode_t smode);
        void onAtmosCapabilitiesChanged(dsATMOSCapability_t atmosCapability, bool status);
        void ReevaluateAtmosExperience();
        void OnHdmiInEventHotPlug(int port, bool isConnected);
        void ReportAudioDevicePowerStatus(const int powerStatus);
        void Dispatch(Event ev, const ParamsType& params);
        void dispatchEvent(Event ev, const ParamsType& params);

    private:
        mutable Core::CriticalSection _adminLock;

        // Last value reported; used only to detect changes for notification. Guarded by _adminLock.
        mutable bool _dolbyAtmosExperience{false};

        // Observer list
        std::list<Exchange::IAudioOutput::INotification*> _observers;

        // DS HAL event listener
        DsAudioPortNotification _dsAudioPortNotification{*this};
        DsHdmiInNotification _dsHdmiInNotification{*this};
        bool _registeredDsEventHandlers{false};

        // Only the first ReportAudioDevicePowerStatus after boot triggers a re-evaluation; later reports are frequent.
        std::atomic<bool> _externalArcEnabled{false};

        // HdmiCecSink (optional) and its notification
        Exchange::IHdmiCecSink* _hdmiCecSink{nullptr};
        Core::Sink<HdmiCecSinkNotification> _hdmiCecSinkNotification;
    };

} // namespace Plugin
} // namespace WPEFramework

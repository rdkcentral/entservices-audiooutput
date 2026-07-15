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

#include "AudioOutput.h"

#define API_VERSION_NUMBER_MAJOR 1
#define API_VERSION_NUMBER_MINOR 0
#define API_VERSION_NUMBER_PATCH 0

namespace WPEFramework {

    namespace {

        static Plugin::Metadata<Plugin::AudioOutput> metadata(
            // Version (Major, Minor, Patch)
            API_VERSION_NUMBER_MAJOR, API_VERSION_NUMBER_MINOR, API_VERSION_NUMBER_PATCH,
            // Preconditions
            {},
            // Terminations
            {},
            // Controls
            {}
        );
    }

    namespace Plugin {

        SERVICE_REGISTRATION(AudioOutput, API_VERSION_NUMBER_MAJOR, API_VERSION_NUMBER_MINOR, API_VERSION_NUMBER_PATCH);

        AudioOutput::AudioOutput()
            : _service(nullptr)
            , _connectionId(0)
            , _audioOutput(nullptr)
            , _notification(this)
            , _connectionNotification(this)
        {
            SYSLOG(Logging::Startup, (_T("AudioOutput Constructor")));
        }

        AudioOutput::~AudioOutput()
        {
            SYSLOG(Logging::Shutdown, (string(_T("AudioOutput Destructor"))));
        }

        const string AudioOutput::Initialize(PluginHost::IShell* service)
        {
            string message = "";

            ASSERT(nullptr != service);
            ASSERT(nullptr == _service);
            ASSERT(nullptr == _audioOutput);
            ASSERT(0 == _connectionId);

            SYSLOG(Logging::Startup, (_T("AudioOutput::Initialize: PID=%u"), getpid()));

            _service = service;
            _service->AddRef();
            _service->Register(&_connectionNotification);

            _audioOutput = _service->Root<Exchange::IAudioOutput>(_connectionId, 5000, _T("AudioOutputImplementation"));

            if (nullptr != _audioOutput) {
                _configure = _audioOutput->QueryInterface<Exchange::IConfiguration>();
                if (_configure != nullptr) {
                    uint32_t result = _configure->Configure(service);
                    if (result != Core::ERROR_NONE) {
                        message = _T("AudioOutput could not be configured");
                    }
                } else {
                    message = _T("AudioOutput implementation did not provide a configuration interface");
                }
                Exchange::JAudioOutput::Register(*this, _audioOutput);
                _audioOutput->Register(&_notification);

            } else {
                SYSLOG(Logging::Startup, (_T("AudioOutput::Initialize: Failed to initialise AudioOutput plugin")));
                message = _T("AudioOutput plugin could not be initialised");
            }

            return message;
        }

        void AudioOutput::Deinitialize(PluginHost::IShell* service)
        {
            ASSERT(_service == service);

            SYSLOG(Logging::Shutdown, (string(_T("AudioOutput::Deinitialize"))));

            if (nullptr != _audioOutput) {
                _audioOutput->Unregister(&_notification);
                Exchange::JAudioOutput::Unregister(*this);

                if (_configure != nullptr) {
                    _configure->Release();
                    _configure = nullptr;
                }

                VARIABLE_IS_NOT_USED uint32_t result = _audioOutput->Release();
                _audioOutput = nullptr;

                ASSERT(result == Core::ERROR_DESTRUCTION_SUCCEEDED);
            }

            _connectionId = 0;
	        _service->Unregister(&_connectionNotification);
            _service->Release();
            _service = nullptr;

            SYSLOG(Logging::Shutdown, (string(_T("AudioOutput de-initialised"))));
        }

        string AudioOutput::Information() const
        {
            return string();
        }

        void AudioOutput::Deactivated(RPC::IRemoteConnection* connection)
        {
            if (connection->Id() == _connectionId) {
                ASSERT(nullptr != _service);
                Core::IWorkerPool::Instance().Submit(
                    PluginHost::IShell::Job::Create(_service, PluginHost::IShell::DEACTIVATED, PluginHost::IShell::FAILURE));
            }
        }

    } // namespace Plugin
} // namespace WPEFramework

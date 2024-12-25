// Copyright (C) 2024 Hyunwoo Park
//
// This file is part of trgkASIO.
//
// trgkASIO is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// (at your option) any later version.
//
// trgkASIO is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with ASIO2WASAPI2.  If not, see <http://www.gnu.org/licenses/>.
//

//
// Created by whyask37 on 2024-11-27.
//

#include <Audioclient.h>
#include "WASAPIOutputLoopbackSource.h"
#include "../../utils/logger.h"
#include "../../utils/raiiUtils.h"
#include <tracy/Tracy.hpp>
#include <algorithm>

#define REFTIMES_PER_SEC  10000000

WASAPIOutputLoopbackSource::WASAPIOutputLoopbackSource(const IMMDevicePtr &pSourceDevice, int channelCount,
                                                       int sampleRate,
                                                       bool interceptDefaultOutput)
        : _channelCount(channelCount), _sampleRate(sampleRate), _pSourceDevice(pSourceDevice),
          _pSourceDeviceId(getDeviceId(pSourceDevice)) {


    IAudioClient *pAudioClient_ = nullptr;

    HRESULT hr = pSourceDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **) &pAudioClient_);
    if (FAILED(hr) || !pAudioClient_) {
        mainlog->error(L"{} WASAPIOutputLoopbackSource: pAudioClient->Activate failed: 0x{:08X}", _pSourceDeviceId,
                       (uint32_t) hr);
        throw AppException("Failed to create loopback device");
    }
    _pAudioClient = make_autorelease(pAudioClient_);

    hr = _pAudioClient->GetMixFormat(&_pwfx);
    if (FAILED(hr)) {
        throw AppException("GetMixFormat failed");
    }

    WAVEFORMATEXTENSIBLE waveFormat = {0};
    waveFormat.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    waveFormat.Format.nChannels = channelCount;
    waveFormat.Format.nSamplesPerSec = _pwfx->nSamplesPerSec;
    waveFormat.Format.wBitsPerSample = 32;
    waveFormat.Format.nBlockAlign = waveFormat.Format.wBitsPerSample * waveFormat.Format.nChannels / 8;
    waveFormat.Format.nAvgBytesPerSec = waveFormat.Format.nSamplesPerSec * waveFormat.Format.nBlockAlign;
    waveFormat.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    waveFormat.Samples.wValidBitsPerSample = waveFormat.Format.wBitsPerSample;
    waveFormat.dwChannelMask = (1 << channelCount) - 1;
    waveFormat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;

    CoTaskMemFree(_pwfx);

    hr = _pAudioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK,
            REFTIMES_PER_SEC,
            0,
            (WAVEFORMATEX *) &waveFormat,
            nullptr);

    if (FAILED(hr)) {
        mainlog->error(L"{} WASAPIOutputLoopbackSource: pAudioClient->Initialize failed: 0x{:08X}", _pSourceDeviceId,
                       (uint32_t) hr);
        throw AppException("IsFormatSupported failed");
    }
    _pAudioClient->GetBufferSize(&_bufferSize);
    _tempBuffers.resize(_channelCount);
    for (auto &channelBuffer: _tempBuffers) {
        channelBuffer.resize(_bufferSize);
    }

    // Create capture clinet
    IAudioCaptureClient *pCaptureClient_ = nullptr;
    hr = _pAudioClient->GetService(
            __uuidof(IAudioCaptureClient),
            (void **) &pCaptureClient_);
    if (FAILED(hr)) {
        mainlog->error(L"{} WASAPIOutputLoopbackSource: pAudioClient->GetService failed: 0x{:08X}", _pSourceDeviceId,
                       (uint32_t) hr);
        throw AppException("GetService failed");
    }
    _pAudioCaptureClient = make_autorelease(pCaptureClient_);

    mainlog->info(L"{} WASAPIOutputLoopbackSource resample: {} -> {}",
                  _pSourceDeviceId,
                  waveFormat.Format.nSamplesPerSec,
                  _sampleRate);
    for (int ch = 0; ch < _channelCount; ch++) {
        _audioBuffer.emplace_back((_bufferSize + 1024) * 2);
        _resamplers.push_back(
                std::make_unique<r8b::CDSPResampler24>(waveFormat.Format.nSamplesPerSec, _sampleRate,
                                                       (int) _bufferSize));
    }

    if (interceptDefaultOutput) {
        auto pPrevOutputDevice = getDefaultOutputDevice();
        auto interceptData = std::make_unique<OutputInterceptData>();
        interceptData->_prevOutputDeviceId = getDeviceId(pPrevOutputDevice);
        interceptData->_prevOutputVolume = std::make_unique<VolumeRAII>(pPrevOutputDevice);
        interceptData->_sourceDeviceVolume = std::make_unique<VolumeRAII>(pSourceDevice);

        float sourceVolume;
        {
            bool sourceMuted;
            if (interceptData->_prevOutputVolume->get(&sourceMuted, &sourceVolume)) {
                if (sourceMuted) sourceVolume = 0.0f;
            } else {
                mainlog->warn(
                        "Failed to get volume value of the intercepted output device. Silencing to prevent unwanted noise");
                sourceVolume = 0.0f;
            }
        }
        interceptData->_sourceDeviceVolume->setMute(true);
        interceptData->_sourceDeviceVolume->setVolume(sourceVolume);

        _interceptData = std::move(interceptData);
        setDefaultOutputDeviceId(_pSourceDeviceId);
        _volumeSyncThread = std::make_unique<std::thread>(&WASAPIOutputLoopbackSource::_volumeSyncThreadProc, this);
    }

    _pAudioClient->Start();
    _fetchThread = std::make_unique<std::thread>(&WASAPIOutputLoopbackSource::_fetchThreadProc, this);
}

WASAPIOutputLoopbackSource::~WASAPIOutputLoopbackSource() {
    _stopThreads = true;
    _fetchThread->join();
    _volumeSyncThread->join();
    _pAudioClient->Stop();
    if (_interceptData) {
        setDefaultOutputDeviceId(_interceptData->_prevOutputDeviceId);
        _interceptData = nullptr;
    }
}

void WASAPIOutputLoopbackSource::_fetchThreadProc() {
    ZoneScoped;

    UINT32 numFramesAvailable;
    UINT32 packetLength = 0;
    BYTE *pData;
    DWORD flags;

    std::vector<double *> outputSamples(_channelCount);
    std::vector<int> outputLength(_channelCount);

    // Vector for getting audio data
    std::vector<std::vector<double>> audioInputBuffers(_channelCount);
    for (auto &channelBuffer: audioInputBuffers) {
        channelBuffer.resize(_bufferSize);
    }

    // Fetch all
    while (!_stopThreads) {
        _pAudioCaptureClient->GetNextPacketSize(&packetLength);
        mainlog->debug(L"{} WASAPIOutputLoopbackSource::render: GetNextPacketSize {}", _pSourceDeviceId, packetLength);
        while (packetLength != 0) {
            _pAudioCaptureClient->GetBuffer(
                    &pData,
                    &numFramesAvailable,
                    &flags, nullptr, nullptr);

            // Buffer upgrade..
            if (audioInputBuffers[0].size() < numFramesAvailable) {
                ZoneScopedN("Buffer upgrade");
                mainlog->warn(
                        L"{} WASAPIOutputLoopbackSource::render: buffer size inadequate: tempBuffer.size({}) < numFramesAvailable({})",
                        _pSourceDeviceId, audioInputBuffers.size(), numFramesAvailable);
                for (auto &channelBuffer: audioInputBuffers) {
                    channelBuffer.resize(numFramesAvailable);
                }
            }

            // Fetch data
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                ZoneScopedN("Fetch audio data - silent");
                mainlog->debug(L"{} WASAPIOutputLoopbackSource::render fetched buffer size {} [silent]",
                               _pSourceDeviceId,
                               numFramesAvailable);
                for (int ch = 0; ch < _channelCount; ch++) {
                    std::fill(audioInputBuffers[ch].data(), audioInputBuffers[ch].data() + numFramesAvailable, 0.0);
                }
            } else {
                ZoneScopedN("Fetch audio data");
                for (int ch = 0; ch < _channelCount; ch++) {
                    auto in = reinterpret_cast<int32_t *>(pData) + ch;
                    auto pStart = audioInputBuffers[ch].data();
                    auto pEnd = audioInputBuffers[ch].data() + numFramesAvailable;
                    for (auto p = pStart; p != pEnd; p++) {
                        *p = *in / 2147483648.0;
                        in += _channelCount;
                    }
                }
                mainlog->debug(L"{} WASAPIOutputLoopbackSource::render fetched buffer size {}", _pSourceDeviceId,
                               numFramesAvailable);
            }

            {
                ZoneScopedN("Resample & enqueue");
                UINT32 resamplerInputPos = 0;
                while (resamplerInputPos < numFramesAvailable) {
                    auto packetSize = std::min(numFramesAvailable - resamplerInputPos, _bufferSize);

                    // Resample outputs
                    for (int ch = 0; ch < _channelCount; ch++) {
                        outputLength[ch] = _resamplers[ch]->process(
                                audioInputBuffers[ch].data() + resamplerInputPos,
                                packetSize,
                                outputSamples[ch]);
                    }

                    // Enqueue
                    {
                        std::lock_guard guard(_audioBufferLock);
                        for (int ch = 0; ch < _channelCount; ch++) {
                            if (!_audioBuffer[ch].push(outputSamples[ch], outputLength[ch])) {
                                mainlog->debug(
                                        L"{} WASAPIOutputLoopbackSource::render: ringbuffer[{}] overflow - capacity {}, size {}, new {}",
                                        _pSourceDeviceId, ch, _audioBuffer[0].capacity(), _audioBuffer[0].size(),
                                        outputLength[ch]);
                            }
                        }
                    }

                    resamplerInputPos += packetSize;
                }
            }

            _pAudioCaptureClient->ReleaseBuffer(numFramesAvailable);
            _pAudioCaptureClient->GetNextPacketSize(&packetLength);
        }

        Sleep(1);
    }
}

void WASAPIOutputLoopbackSource::_volumeSyncThreadProc() {
    if (!_interceptData) return;

    float prevSourceDeviceVolume = -1e9;

    while (!_stopThreads) {
        // Sync volume
        float sourceDeviceVolume;
        _interceptData->_sourceDeviceVolume->get(nullptr, &sourceDeviceVolume);
        if (prevSourceDeviceVolume != sourceDeviceVolume) {
            _interceptData->_prevOutputVolume->setVolume(sourceDeviceVolume);
            _interceptData->_sourceDeviceVolume->setMute(true);
            prevSourceDeviceVolume = sourceDeviceVolume;
        }
        Sleep(10);
    }
}

void WASAPIOutputLoopbackSource::render(int64_t currentFrame, std::vector<std::vector<int32_t>> *outputBuffer) {
    UINT32 numFramesAvailable;
    UINT32 packetLength = 0;
    BYTE *pData;
    DWORD flags;

    // Render
    auto outputSize = outputBuffer->at(0).size();
    if (_audioBuffer[0].size() < outputSize) {
        mainlog->warn(
                L"{} WASAPIOutputLoopbackSource::render: Capture not yet filled: audioBuffer.size({}), outputBuffer.size({})",
                _pSourceDeviceId, _audioBuffer[0].size(), outputSize);
    }

    if (outputSize > _tempBuffers.size()) {
        _tempBuffers.resize(outputSize);
    }

    mainlog->debug(L"{} WASAPIOutputLoopbackSource::render: ringbuffer, size {}", _pSourceDeviceId,
                   _audioBuffer[0].size());

    {
        std::lock_guard guard(_audioBufferLock);
        for (unsigned ch = 0; ch < _channelCount; ch++) {
            _audioBuffer[ch].get(_tempBuffers[ch].data(), outputSize);
            auto &outputBufferChannel = outputBuffer->at(ch);
            for (int i = 0; i < outputSize; i++) {
                outputBufferChannel[i] += (int) round(_tempBuffers[ch][i] * (1 << 23));
            }
        }
    }
}

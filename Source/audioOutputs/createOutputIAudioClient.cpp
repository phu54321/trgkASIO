// Copyright (C) 2023 Hyun Woo Park
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
// along with trgkASIO.  If not, see <http://www.gnu.org/licenses/>.


#include "createOutputIAudioClient.h"
#include "../utils/raiiUtils.h"
#include "../utils/WASAPIUtils.h"
#include <spdlog/spdlog.h>
#include "../utils/logger.h"
#include <mmdeviceapi.h>
#include <Audioclient.h>
#include <tracy/Tracy.hpp>
#include "../pref/UserPref.h"

static void dumpErrorWaveFormatEx(const char *varname, const WAVEFORMATEX &pWaveFormat) {
    mainlog->error("    : {}.wFormatTag: {}", varname, pWaveFormat.wFormatTag);
    mainlog->error("    : {}.channelCount: {}", varname, pWaveFormat.nChannels);
    mainlog->error("    : {}.nSamplesPerSec: {}", varname, pWaveFormat.nSamplesPerSec);
    mainlog->error("    : {}.nAvgBytesPerSec: {}", varname, pWaveFormat.nAvgBytesPerSec);
    mainlog->error("    : {}.nBlockAlign: {}", varname, pWaveFormat.nBlockAlign);
    mainlog->error("    : {}.cbSize: {}", varname, pWaveFormat.cbSize);
}

static std::shared_ptr<IAudioClient> createOutputIAudioClient(
        UserPrefPtr pref,
        const std::shared_ptr<IMMDevice> &pDevice,
        WASAPIMode mode,
        WAVEFORMATEX *pWaveFormat) {
    ZoneScoped;

    if (!pDevice || !pWaveFormat) {
        return nullptr;
    }

    // WASAPI flags
    auto shareMode = (mode == WASAPIMode::Exclusive) ? AUDCLNT_SHAREMODE_EXCLUSIVE : AUDCLNT_SHAREMODE_SHARED;
    auto streamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST;

    ////

    HRESULT hr;
    auto deviceId = getDeviceId(pDevice);
    auto deviceFriendlyName = getDeviceFriendlyName(pDevice);

    IAudioClient *pAudioClient_ = nullptr;
    hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **) &pAudioClient_);
    if (FAILED(hr) || !pAudioClient_) {
        mainlog->error(L"{} pAudioClient->Activate failed: 0x{:08X}", deviceId, (uint32_t) hr);
        return nullptr;
    }
    auto pAudioClient = make_autorelease(pAudioClient_);

    WAVEFORMATEX *closestMatch;
    hr = pAudioClient->IsFormatSupported(shareMode, pWaveFormat, &closestMatch);
    if (FAILED(hr)) {
        mainlog->error(L"{} pAudioClient->IsFormatSupported failed: 0x{:08X}", deviceId, (uint32_t) hr);
        if (closestMatch) {
            dumpErrorWaveFormatEx("ClosestMatch", *closestMatch);
            CoTaskMemFree(closestMatch);
        }
        return nullptr;
    }

    REFERENCE_TIME bufferDuration;
    if (mode == WASAPIMode::Exclusive) {
        const auto &durationOverride = pref->durationOverride;

        auto it = durationOverride.find(deviceId);
        if (it == durationOverride.end()) {
            it = durationOverride.find(deviceFriendlyName);
        }
        if (it != durationOverride.end()) {
            bufferDuration = it->second;
        } else {
            REFERENCE_TIME minBufferDuration, defaultDuration;
            hr = pAudioClient->GetDevicePeriod(&defaultDuration, &minBufferDuration);
            if (FAILED(hr)) return nullptr;
            mainlog->info(L"{} minimum duration {} default duration {}", deviceId, minBufferDuration, defaultDuration);
            bufferDuration = minBufferDuration;
        }
    } else {
        bufferDuration = 0;
    }

    mainlog->debug(L"{} pAudioClient->Initialize: bufferDuration {:.1f}ms", deviceId,
                   (double) bufferDuration / 10000.0);

    hr = pAudioClient->Initialize(
            shareMode,
            streamFlags,
            bufferDuration,
            bufferDuration,
            pWaveFormat,
            nullptr);

    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        // https://learn.microsoft.com/en-US/windows/win32/api/audioclient/nf-audioclient-iaudioclient-initialize
        mainlog->debug(L"{} pAudioClient->Initialize: AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED -> re-initializing",
                       deviceId);

        UINT32 nextHighestAlignedBufferSize = 0;
        hr = pAudioClient->GetBufferSize(&nextHighestAlignedBufferSize);
        if (FAILED(hr)) return nullptr;
        auto alignedBufferDuration = (REFERENCE_TIME) lround(
                10000.0 * 1000 / pWaveFormat->nSamplesPerSec * nextHighestAlignedBufferSize);
        mainlog->debug(L"{} nextHighestAlignedBufferSize {}, alignedBufferDuration {}", deviceId,
                       nextHighestAlignedBufferSize,
                       alignedBufferDuration);

        pAudioClient = nullptr;
        hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **) &pAudioClient_);
        if (FAILED(hr) || !pAudioClient_) {
            mainlog->error(L"{} pAudioClient->Activate failed: 0x{:08X}", deviceId, (uint32_t) hr);
            return nullptr;
        }
        pAudioClient = make_autorelease(pAudioClient_);

        hr = pAudioClient->Initialize(
                shareMode,
                streamFlags,
                alignedBufferDuration,
                alignedBufferDuration,
                pWaveFormat,
                nullptr);
    }

    if (FAILED(hr)) {
        mainlog->error(L"{} pAudioClient->Initialize failed: 0x{:08X}", deviceId, (uint32_t) hr);
        dumpErrorWaveFormatEx("pWaveFormat", *pWaveFormat);
        return nullptr;
    }

    return pAudioClient;
}

bool FindOutputStreamFormat(
        const std::shared_ptr<IMMDevice> &pDevice,
        UserPrefPtr pref,
        int sampleRate,
        WASAPIMode mode,
        WAVEFORMATEXTENSIBLE *pwfxt,
        std::shared_ptr<IAudioClient> *ppAudioClient) {

    ZoneScoped;

    if (!pDevice) return false;

    auto deviceId = getDeviceId(pDevice);
    auto channelCount = pref->channelCount;

    mainlog->debug(TEXT("{} FindOutputStreamFormat: channelCount {}, sampleRate {}, mode {}"),
                   deviceId,
                   channelCount,
                   sampleRate,
                   mode == WASAPIMode::Exclusive ? L"Exclusive" : L"Shared");

    // create a reasonable channel mask
    DWORD dwChannelMask = (1 << channelCount) - 1;
    WAVEFORMATEXTENSIBLE waveFormat = {0};

    //try 32-bit first
    mainlog->debug(TEXT("{} triyng 32bit"), deviceId);
    waveFormat.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    waveFormat.Format.nChannels = channelCount;
    waveFormat.Format.nSamplesPerSec = sampleRate;
    waveFormat.Format.wBitsPerSample = 32;
    waveFormat.Format.nBlockAlign = waveFormat.Format.wBitsPerSample * waveFormat.Format.nChannels / 8;
    waveFormat.Format.nAvgBytesPerSec = waveFormat.Format.nSamplesPerSec * waveFormat.Format.nBlockAlign;
    waveFormat.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    waveFormat.Samples.wValidBitsPerSample = waveFormat.Format.wBitsPerSample;
    waveFormat.dwChannelMask = dwChannelMask;
    waveFormat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;

    auto pAudioClient = createOutputIAudioClient(pref, pDevice, mode, (WAVEFORMATEX *) &waveFormat);
    if (pAudioClient) goto Finish;

    //try 24-bit-in-32bit next
    mainlog->debug(TEXT("{} triyng 24bit-in-32bit"), deviceId);
    waveFormat.Samples.wValidBitsPerSample = 24;
    pAudioClient = createOutputIAudioClient(pref, pDevice, mode, (WAVEFORMATEX *) &waveFormat);
    if (pAudioClient) goto Finish;

    //finally, try 16-bit
    mainlog->debug(TEXT("{} triyng 16bit"), deviceId);
    waveFormat.Format.wBitsPerSample = 16;
    waveFormat.Format.nBlockAlign = waveFormat.Format.wBitsPerSample * waveFormat.Format.nChannels / 8;
    waveFormat.Format.nAvgBytesPerSec = waveFormat.Format.nSamplesPerSec * waveFormat.Format.nBlockAlign;
    waveFormat.Samples.wValidBitsPerSample = waveFormat.Format.wBitsPerSample;
    pAudioClient = createOutputIAudioClient(pref, pDevice, mode, (WAVEFORMATEX *) &waveFormat);
    if (pAudioClient) goto Finish;

    Finish:
    bool bSuccess = (pAudioClient != nullptr);
    if (bSuccess) {
        if (pwfxt)
            memcpy_s(pwfxt, sizeof(WAVEFORMATEXTENSIBLE), &waveFormat, sizeof(WAVEFORMATEXTENSIBLE));
        if (ppAudioClient)
            *ppAudioClient = pAudioClient;
    }
    return bSuccess;
}

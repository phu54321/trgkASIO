// Copyright (C) 2023 Hyunwoo Park
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
//

#include <cstring>
#include <Windows.h>
#include <spdlog/spdlog.h>
#include "TrgkASIOImpl.h"
#include "res/resource.h"
#include "utils/json.hpp"
#include "audioOutputs/WASAPIOutput/createOutputIAudioClient.h"
#include "utils/raiiUtils.h"
#include "RunningState.h"
#include "PreparedState.h"
#include "utils/logger.h"
#include "utils/hexdump.h"
#include <tracy/Tracy.hpp>


using json = nlohmann::json;

static const ASIOSampleType sampleType = ASIOSTInt32LSB;

TrgkASIOImpl::TrgkASIOImpl(void *sysRef)
        : _pref(loadUserPref()) {
    ZoneScoped;

    mainlog->set_level(_pref->logLevel);
    mainlog->info("Starting trgkASIOImpl...");

    CoInitialize(nullptr);

    auto targetDeviceIdList = _pref->deviceIdList;

    auto defaultDeviceId = getDeviceId(getDefaultOutputDevice());
    auto devices = getIMMDeviceList();

    std::vector<std::wstring> deviceIdList;
    std::transform(devices.begin(), devices.end(), std::back_inserter(deviceIdList), getDeviceId);
    std::vector<std::wstring> friendlyNameList;
    std::transform(devices.begin(), devices.end(), std::back_inserter(friendlyNameList), getDeviceFriendlyName);


    mainlog->info("Will find these {} devices", targetDeviceIdList.size());
    for (int i = 0; i < targetDeviceIdList.size(); i++) {
        if (targetDeviceIdList[i].empty() || targetDeviceIdList[i] == L"(default device)") {
            targetDeviceIdList[i] = defaultDeviceId;
            mainlog->info(L" - Target #{:02d}: default output device {}", i, defaultDeviceId);
        } else {
            mainlog->info(L" - Target #{:02d}: {}", i, targetDeviceIdList[i]);
        }
    }

    mainlog->info("Enumerating devices - Total {} device found", devices.size());
    std::map<std::wstring, IMMDevicePtr> deviceMap;
    for (int i = 0; i < devices.size(); i++) {
        mainlog->info(L" - Device #{:02d}: {} {}", i, friendlyNameList[i], deviceIdList[i]);
        for (const auto &id: targetDeviceIdList) {
            auto &device = devices[i];
            if (id == deviceIdList[i] || id == friendlyNameList[i]) {
                mainlog->info("   : Matched");
                deviceMap[id] = device;
                break;
            }
        }
    }

    // Put _pDeviceList with order of targetDeviceIdList
    mainlog->info("Total {} device matched", deviceMap.size());
    for (const auto &id: targetDeviceIdList) {
        auto it = deviceMap.find(id);
        if (it != deviceMap.end()) {
            _pDeviceList.push_back(it->second);
        }
    }


    if (_pDeviceList.empty()) {
        throw AppException("No target device(s) found...");
    }
}

TrgkASIOImpl::~TrgkASIOImpl() {
    ZoneScoped;

    mainlog->info("Stopping trgkASIO...");
    mainlog->flush();

    stop();
    disposeBuffers();
}



/////

ASIOError TrgkASIOImpl::getChannels(long *numInputChannels, long *numOutputChannels) {
    ZoneScoped;

    if (numInputChannels) *numInputChannels = 0;
    if (numOutputChannels) *numOutputChannels = _pref->channelCount;
    return ASE_OK;
}

ASIOError TrgkASIOImpl::getSampleRate(ASIOSampleRate *sampleRate) {
    ZoneScoped;

    if (!sampleRate) return ASE_InvalidParameter;
    *sampleRate = _sampleRate;
    return ASE_OK;
}

ASIOError TrgkASIOImpl::canSampleRate(ASIOSampleRate _sampleRate) {
    ZoneScoped;

    int sampleRate = static_cast<int>(_sampleRate);
    for (int i = 0; i < _pDeviceList.size(); i++) {
        auto &device = _pDeviceList[i];
        auto mode = (i == 0) ? WASAPIMode::Exclusive : WASAPIMode::Shared;
        if (!FindOutputStreamFormat(device, _pref, sampleRate, mode))
            return ASE_NoClock;
    }
    return ASE_OK;
}

ASIOError TrgkASIOImpl::setSampleRate(ASIOSampleRate sampleRate) {
    ZoneScoped;

    mainlog->debug("setSampleRate: {} ( {} )", sampleRate, hexdump(&sampleRate, sizeof(sampleRate)));
    if (sampleRate == _sampleRate) return ASE_OK;

    auto err = canSampleRate(sampleRate);
    if (err != ASE_OK) {
        mainlog->debug(L"canSampleRate: {} (error)", err);
        return err;
    }

    _sampleRate = (int) sampleRate;

    if (_preparedState) {
        _preparedState->requestReset();
    }

    return ASE_OK;
}

///////////////

// all buffer sizes are in frames

static const char *knownChannelNames[] =
        {
                "Front left",
                "Front right",
                "Front center",
                "Low frequency",
                "Back left",
                "Back right",
                "Front left of center",
                "Front right of center",
                "Back center",
                "Side left",
                "Side right",
        };

ASIOError TrgkASIOImpl::getChannelInfo(ASIOChannelInfo *info) {
    ZoneScoped;

    if (info->isInput) return ASE_InvalidParameter;
    if (info->channel < 0 || info->channel >= _pref->channelCount) return ASE_InvalidParameter;

    info->type = sampleType;
    info->channelGroup = 0;
    info->isActive = _preparedState ? ASIOTrue : ASIOFalse;

    strcpy_s(info->name,
             (info->channel < sizeof(knownChannelNames) / sizeof(knownChannelNames[0]))
             ? knownChannelNames[info->channel]
             : "Unknown");

    return ASE_OK;
}

ASIOError TrgkASIOImpl::createBuffers(
        ASIOBufferInfo *bufferInfos,
        long numChannels,
        long bufferSize,
        ASIOCallbacks *callbacks) {

    ZoneScoped;

    mainlog->debug("createBuffers: numChannels {}, bufferSize {}", numChannels, bufferSize);

    // Check parameters
    if (!callbacks) return ASE_InvalidParameter;
    if (numChannels < 0 || numChannels > _pref->channelCount) return ASE_InvalidParameter;
    for (int i = 0; i < numChannels; i++) {
        ASIOBufferInfo &info = bufferInfos[i];
        if (info.isInput || info.channelNum < 0 || info.channelNum >= _pref->channelCount)
            return ASE_InvalidMode;
    }

    // dispose exiting buffers
    disposeBuffers();

    // Allocate!
    _bufferSize = bufferSize;
    _preparedState = std::make_shared<PreparedState>(_pDeviceList, _sampleRate, _bufferSize, _pref, callbacks);
    _preparedState->InitASIOBufferInfo(bufferInfos, numChannels);

    return ASE_OK;
}

ASIOError TrgkASIOImpl::disposeBuffers() {
    ZoneScoped;
    stop();

    // wait for the play thread to finish
    _preparedState = nullptr;
    return ASE_OK;
}



////////////

ASIOError TrgkASIOImpl::start() {
    ZoneScoped;

    if (!_preparedState) return ASE_NotPresent;
    return _preparedState->start() ? ASE_OK : ASE_HWMalfunction;
}

ASIOError TrgkASIOImpl::outputReady() {
    ZoneScoped;
    if (_preparedState) {
        _preparedState->outputReady();
    }
    return ASE_OK;
}


ASIOError TrgkASIOImpl::stop() {
    ZoneScoped;

    if (_preparedState) {
        _preparedState->stop();
    }
    return ASE_OK;
}


////////
// auxillary functions


ASIOError TrgkASIOImpl::getSamplePosition(ASIOSamples *sPos, ASIOTimeStamp *tStamp) {
    if (!_preparedState)
        return ASE_NotPresent;

    return _preparedState->getSamplePosition(sPos, tStamp);
}

ASIOError TrgkASIOImpl::getLatencies(long *_inputLatency, long *_outputLatency) {
    if (!_preparedState)
        return ASE_NotPresent;
    if (_inputLatency)
        *_inputLatency = _bufferSize;
    if (_outputLatency)
        *_outputLatency = 2 * _bufferSize;
    return ASE_OK;
}

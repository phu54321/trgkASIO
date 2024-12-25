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



#pragma once

#ifndef TRGKASIO_TRGKASIOIMPL_H
#define TRGKASIO_TRGKASIOIMPL_H

#include <Windows.h>
#include <thread>
#include <atomic>
#include "utils/AppException.h"
#include "audioOutputs/AudioOutput.h"
#include "pref/UserPref.h"

#include "asiosys.h"
#include "asio.h"
#include "utils/WASAPIUtils.h"


struct PreparedState;

class RunningState;

class TrgkASIOImpl {
public:
    explicit TrgkASIOImpl(void *sysRef);

    ~TrgkASIOImpl();

    ASIOError getChannels(long *numInputChannels, long *numOutputChannels);

    ASIOError getLatencies(long *inputLatency, long *outputLatency);

    ASIOError canSampleRate(ASIOSampleRate sampleRate);

    ASIOError getSampleRate(ASIOSampleRate *sampleRate);

    ASIOError setSampleRate(ASIOSampleRate sampleRate);

    ASIOError getSamplePosition(ASIOSamples *sPos, ASIOTimeStamp *tStamp);

    ASIOError getChannelInfo(ASIOChannelInfo *info);

    ASIOError createBuffers(ASIOBufferInfo *bufferInfos, long numChannels,
                            long bufferSize, ASIOCallbacks *callbacks);

    ASIOError start();

    ASIOError stop();

    ASIOError disposeBuffers();

    ASIOError outputReady();

private:
    UserPrefPtr _pref;
    int _sampleRate = 48000;
    int _bufferSize = 1024;
    std::vector<IMMDevicePtr> _pDeviceList;
    std::shared_ptr<PreparedState> _preparedState;
};

#endif //TRGKASIO_TRGKASIOIMPL_H

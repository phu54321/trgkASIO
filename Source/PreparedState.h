/*  trgkASIO Universal ASIO Driver

    Copyright (C) 2017 Lev Minkovsky
    Copyright (C) 2023 Hyunwoo Park (phu54321@naver.com) - modifications

    This file is part of trgkASIO.

    trgkASIO is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    trgkASIO is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with trgkASIO; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/


#ifndef TRGKASIO_PREPAREDSTATE_H
#define TRGKASIO_PREPAREDSTATE_H

#include <memory>
#include <vector>
#include "asiosys.h"
#include "asio.h"
#include "audioOutputs/AudioOutput.h"
#include "utils/WASAPIUtils.h"
#include "TrgkASIOImpl.h"

class RunningState;

class PreparedState {
    friend class RunningState;

public:
    PreparedState(
            const std::vector<IMMDevicePtr> &pDeviceList,
            int sampleRate,
            int bufferSize,
            UserPrefPtr pref,
            ASIOCallbacks *callbacks
    );

    ~PreparedState();

    void InitASIOBufferInfo(ASIOBufferInfo *infos, int infoCount);

    bool start();

    bool stop();

public:
    void outputReady();

    void requestReset();

    void bufferSwitch(long doubleBufferIndex, ASIOBool directProcess);

    ASIOError getSamplePosition(ASIOSamples *sPos, ASIOTimeStamp *tStamp) const;

private:
    const int _bufferSize;
    const int _sampleRate;
    UserPrefPtr _pref;
    ASIOCallbacks *_callbacks;
    std::vector<IMMDevicePtr> _pDeviceList;

    int _bufferIndex = 0;
    std::vector<std::vector<int32_t>> _buffers[2];

    ASIOTimeStamp _theSystemTime = {0, 0};
    uint64_t _samplePosition = 0;
    std::shared_ptr<RunningState> _runningState;

};


#endif //TRGKASIO_PREPAREDSTATE_H

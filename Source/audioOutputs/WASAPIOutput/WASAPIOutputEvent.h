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



#ifndef TRGKASIO_WASAPIOUTPUTEVENT_H
#define TRGKASIO_WASAPIOUTPUTEVENT_H

#include <Windows.h>
#include <mmdeviceapi.h>
#include <Audioclient.h>

#include <mutex>
#include <memory>
#include <vector>
#include <functional>

#include "../AudioOutput.h"
#include "tracy/Tracy.hpp"
#include "../../utils/RingBuffer.h"
#include "createOutputIAudioClient.h"
#include "../../utils/SynchronizedClock.h"

class WASAPIOutputEvent : public AudioOutput {
public:
    WASAPIOutputEvent(
            const std::shared_ptr<IMMDevice> &pDevice,
            const UserPrefPtr &pref,
            int sampleRate,
            UINT32 inputBufferSize,
            WASAPIMode mode,
            int ringBufferSizeMultiplier,
            SynchronizedClock &clock);

    ~WASAPIOutputEvent();

    /**
     * Push samples to ring central queue. This will be printed to asio.
     * @param buffer `sample = buffer[channel][sampleIndex]`
     */
    void pushSamples(const std::vector<std::vector<int32_t>> &buffer);

    bool started() override { return _started; }

    UINT32 getOutputBufferSize() const { return _outputBufferSize; }

    /**
     * Check how many 'real' samples were pushed to the WASAPI output sink.
     */
    int64_t playedSampleCount() const { return _playedSampleCount; }

private:
    void start();

    void stop();

    bool _started = false;

    static DWORD WINAPI playThread(LPVOID pThis);

private:
    HRESULT LoadData(const std::shared_ptr<IAudioRenderClient> &pRenderClient);

private:
    UINT32 _inputBufferSize;
    UINT32 _outputBufferSize;
    uint64_t _playedSampleCount;

    WASAPIMode _mode;

    std::shared_ptr<IMMDevice> _pDevice;
    std::shared_ptr<IAudioClient> _pAudioClient;
    std::wstring _pDeviceId;
    WAVEFORMATEXTENSIBLE _waveFormat{};

    std::vector<RingBuffer<int32_t>> _ringBufferList;
    std::mutex _ringBufferMutex;
    std::vector<int32_t> _loadDataBuffer;

    HANDLE _stopEvent = nullptr;
    HANDLE _runningEvent = nullptr;

    SynchronizedClock &_clock;
};

#endif //TRGKASIO_WASAPIOUTPUTEVENT_H

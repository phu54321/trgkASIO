/*  ASIO2WASAPI2 Universal ASIO Driver

    Copyright (C) 2017 Lev Minkovsky
    Copyright (C) 2023 Hyunwoo Park (phu54321@naver.com) - modifications

    This file is part of ASIO2WASAPI2.

    ASIO2WASAPI2 is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    ASIO2WASAPI2 is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with ASIO2WASAPI2; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "RunningState.h"
#include "../utils/logger.h"
#include "../utils/raiiUtils.h"
#include <spdlog/spdlog.h>
#include <utility>
#include <cassert>
#include <avrt.h>
#include <timeapi.h>
#include <mmsystem.h>
#include <deque>

#include "../WASAPIOutput/WASAPIOutputEvent.h"
#include "../WASAPIOutput/WASAPIOutputPush.h"
#include "../utils/accurateTime.h"

using namespace std::chrono_literals;

extern HINSTANCE g_hInstDLL;

RunningState::RunningState(PreparedState *p)
        : _preparedState(p),
          _clapRenderer(g_hInstDLL, p->_settings.sampleRate) {
    SPDLOG_TRACE_FUNC;
    std::shared_ptr<WASAPIOutputEvent> mainOutput;

    for (int i = 0; i < p->_pDeviceList.size(); i++) {
        auto &device = p->_pDeviceList[i];
        if (i == 0) {
            auto output = std::make_unique<WASAPIOutputEvent>(
                    device,
                    p->_settings.channelCount,
                    p->_settings.sampleRate,
                    p->_bufferSize);

            _msgWindow.setTrayTooltip(fmt::format(
                    TEXT("Sample rate {}, ASIO input buffer size {}, WASAPI output buffer size {}"),
                    _preparedState->_settings.sampleRate,
                    _preparedState->_settings.bufferSize,
                    output->getOutputBufferSize()));

            _outputList.push_back(std::move(output));
        } else {
            auto output = std::make_unique<WASAPIOutputPush>(
                    device,
                    p->_settings.channelCount,
                    p->_settings.sampleRate,
                    p->_bufferSize
            );
            _outputList.push_back(std::move(output));
        }
    }

    _pollThread = std::thread(RunningState::threadProc, this);
}

RunningState::~RunningState() {
    SPDLOG_TRACE_FUNC;
    signalStop();
    _pollThread.join();
}

void RunningState::signalOutputReady() {
    SPDLOG_TRACE_FUNC;
    {
        mainlog->trace("[RunningState::signalOutputReady] locking mutex");
        std::lock_guard<std::mutex> lock(_mutex);
        _isOutputReady = true;
        mainlog->trace("[RunningState::signalOutputReady] unlocking mutex");
    }
    _notifier.notify_all();
}

void RunningState::signalStop() {
    SPDLOG_TRACE_FUNC;
    {
        mainlog->trace("[RunningState::signalStop] locking mutex");
        std::lock_guard<std::mutex> lock(_mutex);
        _pollStop = true;
        mainlog->trace("[RunningState::signalStop] unlocking mutex");
    }
    _notifier.notify_all();
}

void compress24bitTo32bit(std::vector<std::vector<int32_t>> *outputBuffer) {
    const int32_t overflowPreventer = 5;
    const int32_t compressPadding = (1 << 19) - overflowPreventer;
    const int32_t compressionThresholdHigh =
            (1 << 23) - compressPadding - overflowPreventer;
    const int32_t compressionThresholdLow = -compressionThresholdHigh;

    const auto channelCount = outputBuffer->size();
    const auto bufferSize = outputBuffer->at(0).size();

    for (size_t ch = 0; ch < channelCount; ch++) {
        auto &channelBuffer = outputBuffer->at(ch);
        for (size_t i = 0; i < bufferSize; i++) {
            auto sample = channelBuffer[i];
            int32_t o;
            if (sample > compressionThresholdHigh) {
                auto overflow = sample - compressionThresholdHigh;
                o = compressionThresholdHigh + (int32_t) round(
                        compressPadding * (2 / (1 + exp(-overflow / compressPadding)) - 1));
            } else if (sample < compressionThresholdLow) {
                auto overflow = sample - compressionThresholdLow;
                o = compressionThresholdLow + (int32_t) round(
                        compressPadding * (2 / (1 + exp(-overflow / compressPadding)) - 1));
            } else {
                o = sample;
            }
            channelBuffer[i] = (o << 8);
        }
    }
}


void RunningState::threadProc(RunningState *state) {
    auto &preparedState = state->_preparedState;
    auto bufferSize = preparedState->_settings.bufferSize;
    auto channelCount = preparedState->_settings.channelCount;
    std::vector<std::vector<int32_t>> outputBuffer;
    outputBuffer.resize(preparedState->_settings.channelCount);
    for (auto &buf: outputBuffer) {
        buf.resize(preparedState->_bufferSize);
    }

    // Ask MMCSS to temporarily boost the runThread priority
    // to reduce the possibility of glitches while we play.
    DWORD taskIndex = 0;
    AvSetMmThreadCharacteristics(TEXT("Pro Audio"), &taskIndex);

    TIMECAPS tcaps;
    timeGetDevCaps(&tcaps, sizeof(tcaps));
    timeBeginPeriod(tcaps.wPeriodMin);
    mainlog->info("timeBeginPeriod({})", tcaps.wPeriodMin);

    double lastPollTime = accurateTime();
    double pollInterval = (double) preparedState->_bufferSize / preparedState->_settings.sampleRate;
    bool shouldPoll = true;

    const int keyDownQueueSize = 256;
    struct KeyDownPair {
        double time = 0;
        int pressCount = 0;
    };

    // Fixed-size looping queue
    // This is intentionally designed to overflow after keyDownQueueSize
    // to prevent additional allocation
    std::vector<KeyDownPair> keyDownQueue(keyDownQueueSize);
    int keyDownQueueIndex = 0;

    while (true) {
        auto currentTime = accurateTime();

        // TODO: put this block somewhere appropriate
        {
            // Update keydown queue for clap sound
            auto pressCount = state->_keyListener.pollKeyPressCount();
            if (pressCount > 0) {
                keyDownQueue[keyDownQueueIndex].time = currentTime;
                keyDownQueue[keyDownQueueIndex].pressCount = pressCount;
                keyDownQueueIndex = (keyDownQueueIndex + 1) % keyDownQueueSize;
            }

            // GC old keydown events
            double cutoffTime = currentTime - state->_clapRenderer.getClapSoundLength();
            for (int i = 0; i < keyDownQueueSize; i++) {
                if (keyDownQueue[i].pressCount > 0 && keyDownQueue[i].time < cutoffTime) {
                    keyDownQueue[i].pressCount = 0;
                }
            }
        }


        mainlog->trace("[RunningState::threadProc] Locking mutex");
        std::unique_lock<std::mutex> lock(state->_mutex);

        // Timer event
        if (state->_pollStop) break;
        else if (shouldPoll) {
            TraceHelper _("[RunningState::threadProc] _shouldPoll");
            // Wait for output
            if (!state->_isOutputReady) {
                mainlog->trace("[RunningState::threadProc] unlock mutex d/t notifier wait");
                state->_notifier.wait(lock, [state]() {
                    return state->_isOutputReady || state->_pollStop;
                });
                mainlog->trace("[RunningState::threadProc] re-lock mutex after notifier wait");
            }
            if (state->_pollStop) break;
            state->_isOutputReady = false;
            shouldPoll = false;
            mainlog->trace("[RunningState::threadProc] unlock mutex after flag set");
            lock.unlock();

            assert(preparedState);

            // Put asio main input.
            {
                // Copy data from asio side
                int currentAsioBufferIndex = preparedState->_bufferIndex;
                mainlog->debug("[RunningState::threadProc] Writing {} samples from buffer {}", bufferSize,
                               currentAsioBufferIndex);
                const auto &asioCurrentBuffer = preparedState->_buffers[currentAsioBufferIndex];
                for (size_t ch = 0; ch < channelCount; ch++) {
                    for (size_t i = 0; i < bufferSize; i++) {
                        int32_t sample = asioCurrentBuffer[ch][i];
                        sample >>= 8;  // Scale 32bit to 24bit (To prevent overflow in later steps)`
                        sample -= (sample >> 4);  // multiply 15/16 ( = 0.9375 ) for later compression
                        outputBuffer[ch][i] = sample;
                    }
                }
                mainlog->debug("[RunningState::threadProc] Switching to buffer {}", 1 - currentAsioBufferIndex);
                preparedState->_callbacks->bufferSwitch(1 - currentAsioBufferIndex, ASIOTrue);
                preparedState->_bufferIndex = 1 - currentAsioBufferIndex;
            }

            // Add clap sound
            {
                for (int i = 0; i < keyDownQueueSize; i++) {
                    auto &pair = keyDownQueue[i];
                    if (pair.pressCount > 0) {
                        for (size_t ch = 0; ch < channelCount; ch++) {
                            state->_clapRenderer.render(&outputBuffer[ch], currentTime, pair.time, preparedState->_settings.clapGain * pair.pressCount);
                        }
                    }
                }
            }

            // TODO: add additional processing

            // Rescale & compress output
            compress24bitTo32bit(&outputBuffer);

            // Output
            for (auto &output: state->_outputList) {
                output->pushSamples(outputBuffer);
            }

        } else {
            auto targetTime = lastPollTime + pollInterval;

            mainlog->trace("checkPollTimer: current {:.6f} last {:.6f} interval {:.6f}",
                           currentTime, lastPollTime, pollInterval);

            if (currentTime >= targetTime) {
                lastPollTime += pollInterval;
                mainlog->debug("shouldPoll = true");
                shouldPoll = true;
            } else {
                mainlog->trace("[RunningState::threadProc] Unlock mutex & waiting");
                lock.unlock();

                double remainingTime = targetTime - currentTime;
                Sleep((int) floor(remainingTime / tcaps.wPeriodMin) * tcaps.wPeriodMin);

                while (true) {
                    currentTime = accurateTime();
                    if (currentTime >= targetTime) break;
                    Sleep(0);
                }

                lock.lock();
            }
        }
    }

    timeEndPeriod(tcaps.wPeriodMin);
}

/*  TrgkASIO Universal ASIO Driver

    Copyright (C) 2017 Lev Minkovsky
    Copyright (C) 2023 Hyunwoo Park (phu54321@naver.com) - modifications

    This file is part of TrgkASIO.

    TrgkASIO is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    TrgkASIO is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with TrgkASIO; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "RunningState.h"
#include "utils/logger.h"
#include "utils/intervalBlock.h"
#include "utils/raiiUtils.h"
#include "utils/utf8convert.h"
#include "utils/EventPerSecondCounter.h"
#include <spdlog/spdlog.h>
#include <utility>
#include <cassert>
#include <avrt.h>
#include <timeapi.h>
#include <mmsystem.h>
#include <deque>

#include "audioInputs/keyboardClap/KeyboardClapSource.h"
#include "audioInputs/wasapiOutputLoopback/WASAPIOutputLoopbackSource.h"

#include "audioOutputs/WASAPIOutput/WASAPIOutputEvent.h"
#include "utils/accurateTime.h"
#include "utils/clampSample.h"
#include <tracy/Tracy.hpp>
#include "res/resource.h"

using namespace std::chrono_literals;

extern HINSTANCE g_hInstDLL;


RunningState::RunningState(PreparedState *p)
        : _preparedState(p) {
    ZoneScoped;
    std::shared_ptr<WASAPIOutputEvent> mainOutput;

    auto driverSettings = p->_pref;


    // (*) Since we're passing a reference to _clockNotifier to all _pDeviceList,
    // they must be cleared explicitly on the destructor before _clockNotifier is destructed.
    for (int i = 0; i < p->_pDeviceList.size(); i++) {
        auto &device = p->_pDeviceList[i];
        auto mode = (i == 0) ? WASAPIMode::Exclusive : WASAPIMode::Shared;
        auto output = std::make_shared<WASAPIOutputEvent>(
                device,
                driverSettings,
                p->_sampleRate,
                p->_bufferSize,
                mode,
                2,
                _clock);  // (*)

        if (i == 0) {
            _msgWindow.setTrayTooltip(fmt::format(
                    TEXT("Sample rate {}, ASIO input buffer size {} ({:.2f}ms), WASAPI output buffer size {} ({:.2f}ms)"),
                    p->_sampleRate,
                    p->_bufferSize,
                    1000.0 * p->_bufferSize / p->_sampleRate,
                    output->getOutputBufferSize(),
                    1000.0 * output->getOutputBufferSize() / p->_sampleRate));
            _mainOutput = output;
        }
        _outputList.push_back(std::move(output));
    }

    // Add additional sources
    if (driverSettings->clapGain > 0) {
        _sources.push_back(std::make_shared<KeyboardClapSource>(p->_sampleRate, driverSettings->clapGain));
    }

    // Note: `loopbackInputDevice` might be the same device with `mainOutput`.
    // In this case, it's advisable to just ignore `loopbackInputDevice` rather than
    // not outputing anything to `mainOutput`. For that, `mainOutput` variable should
    // be initialized in prior to `WASAPIOutputLoopbackSource` initialization.
    if (!driverSettings->loopbackInputDevice.empty()) {
        auto pDevices = getIMMDeviceList();
        for (auto &pDevice: pDevices) {
            auto pDeviceId = getDeviceId(pDevice);
            auto pDeviceFriendlyName = getDeviceFriendlyName(pDevice);
            if (pDeviceId == driverSettings->loopbackInputDevice ||
                pDeviceFriendlyName == driverSettings->loopbackInputDevice) {
                try {
                    mainlog->info(L"Initialzing WASAPIOutputLoopbackSource with {}", pDeviceId);
                    _sources.push_back(
                            std::make_shared<WASAPIOutputLoopbackSource>(pDevice, driverSettings->channelCount,
                                                                         p->_sampleRate,
                                                                         driverSettings->autoChangeOutputToLoopback));
                } catch (AppException &e) {
                    mainlog->error(L"{} Exception while initializing WASAPIOutputLoopbackSource with: {}", pDeviceId,
                                   utf8_to_wstring(e.what()));
                }
                break;
            }
        }
    }

    _pollThread = std::thread(RunningState::threadProc, this);
}

RunningState::~RunningState() {
    ZoneScoped;
    signalStop();
    _pollThread.join();

    // (*) Since we're passing a reference to _clockNotifier to all output devices
    // they must be cleared explicitly on the destructor before _clockNotifier is destructed.

    // Clear output devices
    _outputList.clear();
    _mainOutput = nullptr;

    // Clear input devices
    _sources.clear();
}


void RunningState::signalOutputReady() {
    ZoneScoped;
    {
        std::unique_lock lock(_clockStateLock);
        _isOutputReady = true;
    }
    _clock.tick();
}

void RunningState::signalStop() {
    ZoneScoped;
    {
        std::unique_lock lock(_clockStateLock);
        _pollStop = true;
    }
    _clock.tick();
}

void RunningState::threadProc(RunningState *state) {
    auto &preparedState = state->_preparedState;
    auto bufferSize = preparedState->_bufferSize;
    auto channelCount = preparedState->_pref->channelCount;

    std::vector<std::vector<int32_t>> outputBuffer(preparedState->_pref->channelCount);
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
    mainlog->debug("timeBeginPeriod({})", tcaps.wPeriodMin);

    double lastPollTime = accurateTime();
    double pollInterval = (double) preparedState->_bufferSize / preparedState->_sampleRate;
    bool shouldPoll = true;

    IntervalBlock intervalLogBlock(10);
    EventPerSecondCounter mainLoopCounter{L"[RunningState::threadProc] main loop", 10};
    EventPerSecondCounter pollInputCounter{L"[RunningState::threadProc] ASIO input polling", 10};
    int64_t currentOutputFrame = 0;

    auto timerID = timeTickClockTimer(tcaps.wPeriodMin, &state->_clock);

    while (true) {
        if (intervalLogBlock.due()) {
            auto ioLatency = currentOutputFrame - state->_mainOutput->playedSampleCount();
            mainlog->info("[RunningState::threadProc] IO buffer latency: {} frames", ioLatency);
        }
        mainLoopCounter.tick();

        std::unique_lock lock(state->_clockStateLock);
        if (!shouldPoll && !state->_pollStop) {
            lock.unlock();
            state->_clock.wait_for(1);
            lock.lock();
        }

        // Timer event
        if (state->_pollStop) break;
        else if (shouldPoll) {
            ZoneScopedN("[RunningState::threadProc] _shouldPoll");
            pollInputCounter.tick();

            // Wait for output
            if (!state->_isOutputReady) {
                mainlog->trace("[RunningState::threadProc] unlock _mutex d/t notifier wait");
                while (true) {
                    lock.unlock();
                    state->_clock.wait_for(1);
                    if (state->_isOutputReady || state->_pollStop) break;
                    lock.lock();
                }
                mainlog->trace("[RunningState::threadProc] re-lock _mutex after notifier wait");
            }
            if (state->_pollStop) break;
            state->_isOutputReady = false;
            shouldPoll = false;
            mainlog->trace("[RunningState::threadProc] unlock _mutex after flag set");
            lock.unlock();

            // Put asio main input.
            {
                ZoneScopedN("[RunningState::threadProc] _shouldPoll - Polling ASIO data");
                // Copy data from asio side
                int currentAsioBufferIndex = preparedState->_bufferIndex;
                mainlog->trace("[RunningState::threadProc] Writing {} samples from buffer {}", bufferSize,
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
                mainlog->trace("[RunningState::threadProc] Switching to buffer {}", 1 - currentAsioBufferIndex);
                preparedState->bufferSwitch(1 - currentAsioBufferIndex, ASIOTrue);
                preparedState->_bufferIndex = 1 - currentAsioBufferIndex;
            }

            // Check if output should be discarded.
            bool allOutputStarted = true;
            for (auto &output: state->_outputList) {
                if (!output->started()) allOutputStarted = false;
            }

            if (allOutputStarted) {
                // Add additional sources
                for (auto &source: state->_sources) {
                    source->render(currentOutputFrame, &outputBuffer);
                }

                // TODO: add additional processing

                // Rescale & compress output
                for (auto &channel: outputBuffer) {
                    for (int &sample: channel) {
                        double normalizedSample = sample / (double) (1 << 23);
                        sample = (int32_t) (0x7fffffff * clampSample(normalizedSample));
                    }
                }

                // Output
                {
                    ZoneScopedN("[RunningState::threadProc] _shouldPoll - Pushing outputs");
                    for (auto &output: state->_outputList) {
                        output->pushSamples(outputBuffer);
                    }
                }
                currentOutputFrame += bufferSize;
                preparedState->_samplePosition = currentOutputFrame;
            }

            auto ioLatency = currentOutputFrame - state->_mainOutput->playedSampleCount();
            mainlog->trace("[RunningState::threadProc] IO buffer latency: {} frames", ioLatency);
        }

        // Timekeeping
        {
            auto currentTime = accurateTime();
            auto targetTime = lastPollTime + pollInterval;
            mainlog->trace("checkPollTimer: current {:.6f} last {:.6f} interval {:.6f}",
                           currentTime, lastPollTime, pollInterval);

            if (currentTime >= targetTime) {
                lastPollTime += pollInterval;
                mainlog->trace("shouldPoll = true");
                shouldPoll = true;
            }
        }
        Sleep(0);
    }

    timeKillEvent(timerID);
    timeEndPeriod(tcaps.wPeriodMin);
}

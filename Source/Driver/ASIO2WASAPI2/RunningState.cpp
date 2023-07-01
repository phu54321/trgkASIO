//
// Created by whyask37 on 2023-06-30.
//

#include "RunningState.h"
#include "../utils/logger.h"
#include <utility>
#include <cassert>
#include <avrt.h>

RunningState::RunningState(PreparedState *p)
        : _preparedState(p) {
    LOGGER_TRACE_FUNC;

    for (auto &device: p->_pDeviceList) {
        _outputList.push_back(std::make_unique<WASAPIOutput>(
                device,
                p->_settings.nChannels,
                p->_settings.nSampleRate,
                p->_bufferSize));
    }
    _outputList[0]->registerCallback([this]() {
        signalPoll();
    });

    _pollThread = std::thread(RunningState::threadProc, this);
}

RunningState::~RunningState() {
    LOGGER_TRACE_FUNC;
    signalStop();
    _pollThread.join();
}

void RunningState::signalOutputReady() {
    LOGGER_TRACE_FUNC;
    {
        Logger::trace("[RunningState::signalOutputReady] locking mutex");
        std::lock_guard<std::mutex> lock(_mutex);
        _isOutputReady = true;
        Logger::trace("[RunningState::signalOutputReady] unlocking mutex");
    }
    _notifier.notify_all();
}

void RunningState::signalStop() {
    LOGGER_TRACE_FUNC;
    {
        Logger::trace("[RunningState::signalStop] locking mutex");
        std::lock_guard<std::mutex> lock(_mutex);
        _pollStop = true;
        Logger::trace("[RunningState::signalStop] unlocking mutex");
    }
    _notifier.notify_all();
}

void RunningState::signalPoll() {
    LOGGER_TRACE_FUNC;
    {
        Logger::trace("[RunningState::signalPoll] locking mutex");
        std::lock_guard<std::mutex> lock(_mutex);
        _shouldPoll = true;
        Logger::trace("[RunningState::signalPoll] unlocking mutex");
    }
    _notifier.notify_all();
}

void RunningState::threadProc(RunningState *state) {
    auto &_preparedState = state->_preparedState;
    auto bufferSize = state->_preparedState->_settings.bufferSize;

    // Ask MMCSS to temporarily boost the runThread priority
    // to reduce the possibility of glitches while we play.
    DWORD taskIndex = 0;
    AvSetMmThreadCharacteristics(TEXT("Pro Audio"), &taskIndex);


    while (true) {
        Logger::trace("[RunningState::threadProc] Locking mutex");
        std::unique_lock<std::mutex> lock(state->_mutex);
        if (state->_pollStop) break;
        else if (state->_shouldPoll) {
            Logger::trace("[RunningState::threadProc] _shouldPoll");
            // Wait for output
            if (!state->_isOutputReady) {
                Logger::trace("[RunningState::threadProc] unlock mutex d/t notifier wait");
                state->_notifier.wait(lock, [state]() {
                    return state->_isOutputReady || state->_pollStop;
                });
                Logger::trace("[RunningState::threadProc] re-lock mutex after notifier wait");
            }
            if (state->_pollStop) break;
            state->_isOutputReady = false;
            state->_shouldPoll = false;
            Logger::trace("[RunningState::threadProc] unlock mutex after flag set");
            lock.unlock();

            assert(_preparedState);
            int currentBufferIndex = _preparedState->_bufferIndex;
            const auto &currentBuffer = _preparedState->_buffers[currentBufferIndex];
            Logger::debug("[RunningState::threadProc] Writing %d samples from buffer %d", bufferSize,
                          currentBufferIndex);
            for (auto &output: state->_outputList) {
                output->pushSamples(currentBuffer);
            }

            Logger::debug("[RunningState::threadProc] Switching to buffer %d", 1 - currentBufferIndex);
            _preparedState->_callbacks->bufferSwitch(1 - currentBufferIndex, ASIOTrue);
            _preparedState->_bufferIndex = 1 - currentBufferIndex;
        } else {
            Logger::trace("[RunningState::threadProc] Unlock mutex & waiting");
            state->_notifier.wait(lock, [state]() {
                return state->_pollStop || state->_shouldPoll;
            });
        }
    }
}

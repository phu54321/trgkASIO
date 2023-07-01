//
// Created by whyask37 on 2023-06-30.
//

#include "RunningState.h"
#include "../utils/logger.h"
#include <utility>
#include <cassert>

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
        std::lock_guard<std::mutex> lock(_mutex);
        _isOutputReady = true;
    }
    _notifier.notify_all();
}

void RunningState::signalStop() {
    LOGGER_TRACE_FUNC;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _pollStop = true;
    }
    _notifier.notify_all();
}

void RunningState::signalPoll() {
    LOGGER_TRACE_FUNC;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _shouldPoll = true;
    }
    _notifier.notify_all();
}

void RunningState::threadProc(RunningState *state) {
    auto &_preparedState = state->_preparedState;
    auto bufferSize = state->_preparedState->_settings.bufferSize;

    while (true) {
        Logger::trace("[RunningState] Locking mutex");
        std::unique_lock<std::mutex> lock(state->_mutex);
        if (state->_pollStop) break;
        else if (state->_shouldPoll) {
            Logger::trace("[RunningState] Unlock d/t outputReady");

            // Wait for output
            if (!state->_isOutputReady) {
                state->_notifier.wait(lock, [state]() {
                    return state->_isOutputReady || state->_pollStop;
                });
            }
            if (state->_pollStop) break;
            state->_isOutputReady = false;
            state->_shouldPoll = false;
            lock.unlock();

            assert(_preparedState);
            int currentBufferIndex = _preparedState->_bufferIndex;
            const auto &currentBuffer = _preparedState->_buffers[currentBufferIndex];
            Logger::debug("[RunningState] Writing %d samples from buffer %d", bufferSize, currentBufferIndex);
            for (auto &output: state->_outputList) {
                output->pushSamples(currentBuffer);
            }

            Logger::debug("[RunningState] Switching to buffer %d", 1 - currentBufferIndex);
            _preparedState->_callbacks->bufferSwitch(1 - currentBufferIndex, ASIOTrue);
            _preparedState->_bufferIndex = 1 - currentBufferIndex;
        } else {
            Logger::trace("[RunningState] Unlock & waiting");
            state->_notifier.wait(lock, [state]() {
                return state->_pollStop || state->_shouldPoll;
            });
        }
    }
}

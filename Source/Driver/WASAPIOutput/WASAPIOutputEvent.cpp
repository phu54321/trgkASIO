//
// Created by whyask37 on 2023-06-26.
//


#include <Windows.h>
#include <mmdeviceapi.h>
#include <Audioclient.h>
#include <avrt.h>
#include <cassert>
#include <mutex>
#include <cstdlib>

#include "WASAPIOutput.h"
#include "WASAPIOutputEvent.h"
#include "createIAudioClient.h"
#include <spdlog/spdlog.h>
#include "../utils/WASAPIUtils.h"
#include "../utils/raiiUtils.h"
#include "../utils/logger.h"
#include "../utils/AppException.h"

const int ringBufferSizeMultiplier = 3;


WASAPIOutputEvent::WASAPIOutputEvent(
        const std::shared_ptr<IMMDevice> &pDevice,
        int channelNum,
        int sampleRate,
        int bufferSizeRequest,
        std::function<void()> eventCallback)
        : _pDevice(pDevice), _channelNum(channelNum), _sampleRate(sampleRate), _eventCallback(eventCallback) {

    SPDLOG_TRACE_FUNC;
    HRESULT hr;

    _pDeviceId = getDeviceId(pDevice);

    _inputBufferSize = bufferSizeRequest;
    if (!FindStreamFormat(pDevice, channelNum, sampleRate, bufferSizeRequest, WASAPIMode::Event, &_waveFormat,
                          &_pAudioClient)) {
        mainlog->error(L"{} Cannot find suitable stream format for output _pDevice", _pDeviceId);
        throw AppException("FindStreamFormat failed");
    }

    hr = _pAudioClient->GetBufferSize(&_outputBufferSize);
    if (FAILED(hr)) {
        throw AppException("GetBufferSize failed");
    }
    mainlog->info(L"{} WASAPIOutputEvent: - Buffer size: input {}, output {}",
                  _pDeviceId, _inputBufferSize, _outputBufferSize);

    _ringBufferSize = (_inputBufferSize + _outputBufferSize) * ringBufferSizeMultiplier;
    _ringBuffer.resize(channelNum);
    for (int i = 0; i < channelNum; i++) {
        _ringBuffer[i].resize(_ringBufferSize);
        std::fill(_ringBuffer[i].begin(), _ringBuffer[i].end(), 0);
    }

    // TODO: start only when sufficient data is fetched.
    start();
}

WASAPIOutputEvent::~WASAPIOutputEvent() {
    SPDLOG_TRACE_FUNC;

    stop();
    WaitForSingleObject(_runningEvent, INFINITE);
}

void WASAPIOutputEvent::start() {
    SPDLOG_TRACE_FUNC;

    // Wait for previous wasapi thread to close
    WaitForSingleObject(_runningEvent, INFINITE);
    if (!_stopEvent) {
        _stopEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
        CreateThread(nullptr, 0, playThread, this, 0, nullptr);
    }
}

void WASAPIOutputEvent::stop() {
    SPDLOG_TRACE_FUNC;

    if (_stopEvent) {
        SetEvent(_stopEvent);
        CloseHandle(_stopEvent);
        _stopEvent = nullptr;
    }
}


void WASAPIOutputEvent::pushSamples(const std::vector<std::vector<short>> &buffer) {
    assert (buffer.size() == _channelNum);

    mainlog->debug(L"{} pushSamples, rp {} wp {} _ringBufferSize {} _inputBufferSize {}, _outputBufferSize {}",
                   _pDeviceId, _ringBufferReadPos, _ringBufferWritePos,
                   _ringBufferSize, _inputBufferSize, _outputBufferSize);

    if (buffer.size() != _channelNum) {
        mainlog->error(L"{} Invalid channel count: expected {}, got {}", _pDeviceId, _channelNum, buffer.size());
        return;
    }

    if (buffer[0].size() != _inputBufferSize) {
        mainlog->error(L"{} Invalid chunk length: expected {}, got {}", _pDeviceId, _inputBufferSize, buffer[0].size());
        return;
    }

    bool write_overflow = false;
    {
        std::lock_guard<std::mutex> guard(_ringBufferMutex);
        auto &rp = _ringBufferReadPos;
        auto &wp = _ringBufferWritePos;

        if (rp <= wp) {
            // case 1: -----rp+++++++++++wp-------
            if (wp + _inputBufferSize <= _ringBufferSize) {
                // case 1-1 -----rp+++++++++++wp@@@@@wp--
                for (int ch = 0; ch < _channelNum; ch++) {
                    memcpy(
                            _ringBuffer[ch].data() + wp,
                            buffer[ch].data(),
                            _inputBufferSize * sizeof(buffer[ch][0]));
                }
                wp += _inputBufferSize;
                if (wp == _ringBufferSize) wp = 0;
            } else {
                // case 1-1 @@wp--rp+++++++++++wp@@@@@@@@
                auto fillToEndSize = _ringBufferSize - wp;
                auto fillFromStartSize = _inputBufferSize - fillToEndSize;
                if (fillFromStartSize >= rp) {
                    write_overflow = true;
                } else {
                    for (int ch = 0; ch < _channelNum; ch++) {
                        memcpy(
                                _ringBuffer[ch].data() + wp,
                                buffer[ch].data(),
                                fillToEndSize * sizeof(buffer[ch][0]));
                        memcpy(
                                _ringBuffer[ch].data(),
                                buffer[ch].data() + fillToEndSize,
                                fillFromStartSize * sizeof(buffer[ch][0]));
                    }
                    wp = fillFromStartSize;
                }
            }
        } else {
            // case 2: ++++wp--------------rp++++
            if (wp + _inputBufferSize >= rp) {
                write_overflow = true;
            } else {
                for (int ch = 0; ch < _channelNum; ch++) {
                    memcpy(
                            _ringBuffer[ch].data() + wp,
                            buffer[ch].data(),
                            _inputBufferSize * sizeof(buffer[ch][0]));
                }
                wp += _inputBufferSize;
            }
        }
    }

    // Logging may take long, so do this outside of the mutex
    if (write_overflow) {
        mainlog->warn(L"{} [++++++++++] Write overflow!", _pDeviceId);
        return;
    }
}


HRESULT WASAPIOutputEvent::LoadData(const std::shared_ptr<IAudioRenderClient> &pRenderClient) {
    if (!pRenderClient) {
        return E_INVALIDARG;
    }

    SPDLOG_TRACE_FUNC;

    if (_eventCallback) {
        mainlog->trace(L"{} Pulling data from ASIO side", _pDeviceId);
        _eventCallback();
    }

    BYTE *pData;
    HRESULT hr = pRenderClient->GetBuffer(_outputBufferSize, &pData);
    if (FAILED(hr)) {
        mainlog->error(L"{} _pRenderClient->GetBuffer({}) failed, (0x{:08X})", _pDeviceId, _outputBufferSize,
                       (uint32_t) hr);
        return E_FAIL;
    }

    UINT32 sampleSize = _waveFormat.Format.wBitsPerSample / 8;
    assert(sampleSize == 2);

    mainlog->debug(L"{} LoadData, rp {} wp {} ringSize {}", _pDeviceId, _ringBufferReadPos, _ringBufferWritePos,
                   _ringBufferSize);

    bool skipped = false;
    {
        std::lock_guard<std::mutex> guard(_ringBufferMutex);
        size_t &rp = _ringBufferReadPos;
        size_t &wp = _ringBufferWritePos;
        size_t currentDataLength = (wp - rp + _ringBufferSize) % _ringBufferSize;

        if (currentDataLength < _outputBufferSize) {
            memset(pData, 0, sampleSize * _channelNum * _outputBufferSize);
            skipped = true;
        } else {
            for (unsigned channel = 0; channel < _channelNum; channel++) {
                auto out = reinterpret_cast<short *>(pData) + channel;
                short *pStart = _ringBuffer[channel].data() + rp;
                short *pEnd = _ringBuffer[channel].data() + (rp + _outputBufferSize) % _ringBufferSize;
                short *pWrap = _ringBuffer[channel].data() + _ringBufferSize;
                for (short *p = pStart; p != pEnd;) {
                    *out = *p;
                    out += _channelNum;
                    p++;
                    if (p == pWrap) p = _ringBuffer[channel].data();
                }
            }
            rp = (rp + _outputBufferSize) % _ringBufferSize;
        }
    }

    // mainlog->warn may take long, so this logging should be done outside _ringBufferMutex lock.
    if (skipped) {
        mainlog->warn(L"{} [----------] Skipped pushing to wasapi", _pDeviceId);
    }

    pRenderClient->ReleaseBuffer(_outputBufferSize, 0);
    return S_OK;
}

/////

DWORD WINAPI WASAPIOutputEvent::playThread(LPVOID pThis) {
    HRESULT hr;

    auto *pDriver = static_cast<WASAPIOutputEvent *>(pThis);
    pDriver->_runningEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    CExitEventSetter setter(pDriver->_runningEvent);

    auto pAudioClient = pDriver->_pAudioClient;
    BYTE *pData = nullptr;

    hr = CoInitialize(nullptr);
    if (FAILED(hr))
        return -1;

    // Create an event handle and register it for
    // buffer-event notifications.
    HANDLE hEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    auto hEvent = make_autoclose(hEvent_);

    hr = pAudioClient->SetEventHandle(hEvent.get());
    if (FAILED(hr))
        return -1;

    IAudioRenderClient *pRenderClient_ = nullptr;
    hr = pAudioClient->GetService(
            IID_IAudioRenderClient,
            (void **) &pRenderClient_);
    if (FAILED(hr))
        return -1;
    auto pRenderClient = make_autorelease(pRenderClient_);

    // Ask MMCSS to temporarily boost the runThread priority
    // to reduce the possibility of glitches while we play.
    DWORD taskIndex = 0;
    AvSetMmThreadCharacteristics(TEXT("Pro Audio"), &taskIndex);

    // Pre-load the first buffer with data
    // from the audio source before starting the stream.
    hr = pDriver->LoadData(pRenderClient);
    if (FAILED(hr))
        return -1;

    hr = pAudioClient->Start(); // Start playing.
    if (FAILED(hr))
        return -1;

    HANDLE events[2] = {pDriver->_stopEvent, hEvent.get()};
    while ((WaitForMultipleObjects(2, events, FALSE, INFINITE)) ==
           (WAIT_OBJECT_0 + 1)) { // the hEvent is signalled and m_hStopPlayThreadEvent is not
        // Grab the next empty buffer from the audio _pDevice.
        pDriver->LoadData(pRenderClient);
    }

    hr = pAudioClient->Stop(); // Stop playing.
    if (FAILED(hr))
        return -1;

    return 0;
}

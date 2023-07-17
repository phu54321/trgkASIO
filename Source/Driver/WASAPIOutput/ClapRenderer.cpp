// Copyright (C) 2023 Hyunwoo Park
//
// This file is part of ASIO2WASAPI2.
//
// ASIO2WASAPI2 is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// (at your option) any later version.
//
// ASIO2WASAPI2 is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with ASIO2WASAPI2.  If not, see <http://www.gnu.org/licenses/>.
//

#include "ClapRenderer.h"
#include "../utils/ResourceLoad.h"
#include "../res/resource.h"
#include "../utils/logger.h"
#include "../utils/WaveLoad.h"
#include "../lib/r8brain_free_src/CDSPResampler.h"

#include <vector>
#include <mmsystem.h>

/////// IDK the use case, but user MAY replace clap sound resource
// with their own things...


ClapRenderer::ClapRenderer(HMODULE hDLL, int targetSampleRate) {
    try {
        auto clapSoundWAV = loadUserdataResource(hDLL, MAKEINTRESOURCE(IDR_CLAP_MONO));
        _clapSound = loadWaveSound(clapSoundWAV, targetSampleRate);
    } catch (AppException &e) {
        mainlog->error("Cannot load clap sound: {}", e.what());
    }
}

double ClapRenderer::getClapSoundLength() const {
    return (double) _clapSound.audio.size() / (double) _clapSound.sampleRate;
}

void ClapRenderer::render(std::vector<int32_t> *output, double renderTime, double clapStartTime, double gain) const {
    assert(output);

    double clapRelTime = (renderTime - clapStartTime);
    int clapStartSamples = (int) round(clapRelTime * _clapSound.sampleRate);
    mainlog->trace("clapRelTime {}, clapStartSamples {}", clapRelTime, clapStartSamples);

    // Clap hadn't started
    const auto &samples = _clapSound.audio;
    int32_t *outP = output->data();
    for (int i = 0; i < output->size(); i++) {
        auto inPos = i + clapStartSamples;
        if (inPos < 0) continue;
        else if (inPos >= samples.size()) break;
        outP[i] += (int32_t) round(samples[inPos] * gain * (1 << 24));
    }
}

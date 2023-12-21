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



#include "ASIO2WASAPI2Impl.h"
#include "utils/json.hpp"
#include "utils/utf8convert.h"
#include "utils/homeDirFilePath.h"
#include "utils/logger.h"
#include "spdlog/spdlog.h"

using json = nlohmann::json;

const wchar_t *defaultDevices[] = {
        L"(default)",
        L"CABLE Input(VB-Audio Virtual Cable)",
};

DriverSettings &loadDriverSettings() {
    static bool inited = false;
    static DriverSettings ret;
    if (inited) return ret;

    FILE *fp = homeDirFOpen(TEXT("ASIO2WASAPI2.json"), TEXT("rb"));
    if (!fp) {
        // use default
        mainlog->info("ASIO2WASAPI2.json not found. Using default settings");
        for (const wchar_t *device: defaultDevices) {
            ret.deviceIdList.emplace_back(device);
        }

        inited = true;
        return ret;
    }

    try {
        auto j = json::parse(fp);

        ret.channelCount = j.value("channelCount", 2);
        ret.sampleRate = j.value("sampleRate", 48000);
        ret.bufferSize = j.value("bufferSize", 1024);
        ret.clapGain = j.value("clapGain", 0.);
        ret.throttle = j.value("throttle", true);

        // Note:: Declare default log level on logger.cpp
        auto logLevel = j.value("logLevel", "");
        if (logLevel == "trace") mainlog->set_level(spdlog::level::trace);
        if (logLevel == "debug") mainlog->set_level(spdlog::level::debug);
        if (logLevel == "info") mainlog->set_level(spdlog::level::info);
        if (logLevel == "warn") mainlog->set_level(spdlog::level::warn);
        if (logLevel == "error") mainlog->set_level(spdlog::level::err);

        if (!j.contains("deviceId")) {
            for (const wchar_t *device: defaultDevices) {
                ret.deviceIdList.emplace_back(device);
            }
        } else if (j["deviceId"].is_string()) {
            ret.deviceIdList.push_back(utf8_to_wstring(j.value("deviceId", "")));
        } else if (j["deviceId"].is_array()) {
            for (const auto &item: j["deviceId"]) {
                ret.deviceIdList.push_back(utf8_to_wstring(item));
            }
        }

        if (j.contains("durationOverride")) {
            auto &durationOverride = j["durationOverride"];
            if (!durationOverride.is_object()) {
                throw AppException("durationOverride must be an object");
            }

            for (auto it = durationOverride.begin(); it != durationOverride.end(); ++it) {
                std::wstring deviceId = utf8_to_wstring(it.key());
                int override = it.value();
                ret.durationOverride.insert(std::make_pair(deviceId, override));
            }
        }

        inited = true;
        return ret;
    } catch (json::exception &e) {
        mainlog->error("JSON parse failed: {}", e.what());
        throw AppException("JSON parse failed");
    }
}
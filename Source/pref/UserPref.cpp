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



#include "UserPref.h"
#include "../utils/json.hpp"
#include "../utils/utf8convert.h"
#include "../utils/homeDirFilePath.h"
#include "../utils/logger.h"
#include <spdlog/spdlog.h>

using json = nlohmann::json;

const wchar_t *defaultDevices[] = {
        L"(default)",
        L"CABLE Input(VB-Audio Virtual Cable)",
};

UserPrefPtr loadUserPref() {
    FILE *fp = homeDirFOpen(TEXT("ASIO2WASAPI2.json"), TEXT("rb"));
    auto ret = std::make_shared<UserPref>();
    if (!fp) {
        // use default
        mainlog->info("ASIO2WASAPI2.json not found. Using default settings");
        for (const wchar_t *device: defaultDevices) {
            ret->deviceIdList.emplace_back(device);
        }

        return ret;
    }

    try {
        auto j = json::parse(fp);

        ret->channelCount = j.value("channelCount", 2);
        ret->clapGain = j.value("clapGain", 0.);
        ret->throttle = j.value("throttle", true);

        // Note:: Declare default log level on logger.cpp
        auto logLevel = j.value("logLevel", "");
        if (logLevel == "trace") mainlog->set_level(spdlog::level::trace);
        if (logLevel == "debug") mainlog->set_level(spdlog::level::debug);
        if (logLevel == "info") mainlog->set_level(spdlog::level::info);
        if (logLevel == "warn") mainlog->set_level(spdlog::level::warn);
        if (logLevel == "error") mainlog->set_level(spdlog::level::err);

        if (!j.contains("deviceId")) {
            for (const wchar_t *device: defaultDevices) {
                ret->deviceIdList.emplace_back(device);
            }
        } else if (j["deviceId"].is_string()) {
            ret->deviceIdList.push_back(utf8_to_wstring(j.value("deviceId", "")));
        } else if (j["deviceId"].is_array()) {
            for (const auto &item: j["deviceId"]) {
                ret->deviceIdList.push_back(utf8_to_wstring(item));
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
                ret->durationOverride.insert(std::make_pair(deviceId, override));
            }
        }

        return ret;
    } catch (json::exception &e) {
        mainlog->error("JSON parse failed: {}", e.what());
        throw AppException("JSON parse failed");
    }
}


void saveUserPref(UserPrefPtr pref, LPCTSTR saveRelPath) {
    FILE *fp = homeDirFOpen(saveRelPath, TEXT("wb"));
    if (!fp) {
        // use default
        mainlog->error(TEXT("[saveUserPref] homeDirFOpen failed: {}"), saveRelPath);
        return;
    }

    json j;
    j["channelCount"] = pref->channelCount;
    j["clapGain"] = pref->clapGain;
    j["throttle"] = pref->throttle;

    // todo: logLevel

    {
        json jDeviceIdList = json::array();
        for (const auto &s: pref->deviceIdList) {
            jDeviceIdList.push_back(wstring_to_utf8(s));
        }
        j["deviceId"] = jDeviceIdList;
    }

    {
        json jDurationOverride = json::object();
        for (const auto &p: pref->durationOverride) {
            jDurationOverride[wstring_to_utf8(p.first)] = p.second;
        }
        j["durationOverride"] = jDurationOverride;
    }

    fputs(j.dump(2).c_str(), fp);
    fclose(fp);
    return;
}
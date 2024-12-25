// Copyright (C) 2023 Hyunwoo Park
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
// along with ASIO2WASAPI2.  If not, see <http://www.gnu.org/licenses/>.
//

//
// Created by whyask37 on 2023-12-24.
//

#include "DlgDurationOverrideEditor.h"
#include "../res/resource.h"
#include <sstream>
#include <string>
#include <regex>
#include <spdlog/fmt/fmt.h>
#include <windowsx.h>
#include <Audioclient.h>
#include "../utils/w32StringGetter.h"
#include "../utils/WASAPIUtils.h"
#include "../utils/logger.h"

static std::wstring trim(std::wstring str) {
    str.erase(str.find_last_not_of(' ') + 1);         //suffixing spaces
    str.erase(0, str.find_first_not_of(' '));       //prefixing spaces
    return str;
}

static bool parseDurationOverrideLine(const std::wstring &line, std::wstring *deviceId, int *duration) {
    static std::wregex r(L"^(.+):\\s*(\\d+)$");
    std::wsmatch sm;

    if (std::regex_match(line, sm, r)) {
        *deviceId = trim(sm[1].str());
        *duration = std::stoi(sm[2].str());
        return true;
    } else {
        return false;
    }
}

////////////////////

struct OutputLatencyEntry {
    std::wstring deviceId;
    int duration;
    int minimumBufferDuration = 0;
    int defaultBufferDuration = 0;
    std::vector<IMMDevicePtr> deviceList;
    std::vector<std::wstring> deviceIdList;
    std::vector<std::wstring> friendlyNameList;
};

static LPCWSTR listSeparator = L"----------------";

static INT_PTR CALLBACK DlgPromptOutputOverrideEntry(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_INITDIALOG: {
            auto entryPtr = reinterpret_cast<OutputLatencyEntry *>(lParam);
            SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR) entryPtr);

            auto hDeviceList = GetDlgItem(hWnd, IDC_DEVICE);

            entryPtr->deviceList.clear();
            entryPtr->deviceIdList.clear();
            entryPtr->friendlyNameList.clear();

            entryPtr->deviceList = getIMMDeviceList();
            for (size_t i = 0; i < entryPtr->deviceList.size(); i++) {
                auto &d = entryPtr->deviceList[i];
                entryPtr->deviceIdList.push_back(getDeviceId(d));
                entryPtr->friendlyNameList.push_back(getDeviceFriendlyName(d));
            }

            std::vector<std::wstring> candidates;

            for (size_t i = 0; i < entryPtr->deviceList.size(); i++) {
                const auto &deviceFriendlyName = entryPtr->friendlyNameList[i];
                SendMessageW(hDeviceList, CB_ADDSTRING, 0, (LPARAM) deviceFriendlyName.c_str());
            }

            SetDlgItemTextW(hWnd, IDC_DEVICE, entryPtr->deviceId.c_str());
            SetDlgItemInt(hWnd, IDC_EDIT_DURATION, entryPtr->duration, TRUE);

            SendMessageW(hWnd, WM_COMMAND, MAKEWPARAM(IDC_DEVICE, CBN_EDITUPDATE), (LPARAM) hDeviceList);

            return TRUE;
        }

        case WM_CLOSE:
            EndDialog(hWnd, FALSE);
            return TRUE;

        case WM_COMMAND: {
            WORD id = LOWORD(wParam);
            switch (id) {
                case IDC_DEVICE: {
                    auto hDeviceList = (HWND) lParam;
                    std::wstring selectedDeviceIdentifier;

                    switch (HIWORD(wParam)) {
                        case CBN_SELCHANGE: {
                            auto curSel = ComboBox_GetCurSel(hDeviceList);
                            if (curSel != CB_ERR) {
                                selectedDeviceIdentifier = ComboBox_GetWString(hDeviceList, curSel);
                            } else {
                                selectedDeviceIdentifier = getWndText(hDeviceList);
                            }
                            break;
                        }

                        case CBN_EDITUPDATE:
                        case CBN_EDITCHANGE:
                            selectedDeviceIdentifier = getWndText(hDeviceList);
                            break;

                        default:
                            return FALSE;
                    }

                    auto entryPtr = (OutputLatencyEntry *) GetWindowLongPtr(hWnd, GWLP_USERDATA);
                    auto &deviceList = entryPtr->deviceList;

                    IMMDevicePtr selectedDevice;
                    for (size_t i = 0; i < deviceList.size(); i++) {
                        const auto &deviceId = entryPtr->deviceIdList[i];
                        const auto &friendlyName = entryPtr->friendlyNameList[i];
                        if (deviceId == selectedDeviceIdentifier || friendlyName == selectedDeviceIdentifier) {
                            mainlog->debug(L"Selected device {} ({})", friendlyName, deviceId);
                            SetDlgItemTextW(hWnd, IDC_DEVICE_FRIENDLY_NAME, friendlyName.c_str());
                            selectedDevice = deviceList[i];
                            break;
                        }
                    }

                    std::string durationText;
                    if (selectedDevice) {
                        IAudioClient *pAudioClient = nullptr;
                        auto hr = selectedDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                                           (void **) &pAudioClient);
                        if (FAILED(hr) || !pAudioClient) {
                            mainlog->error(L" - pAudioClient->Activate failed: 0x{:08X}", (uint32_t) hr);
                            durationText = "no latency information provided";
                        } else {
                            REFERENCE_TIME defaultBufferDuration, minBufferDuration;
                            pAudioClient->GetDevicePeriod(&defaultBufferDuration, &minBufferDuration);
                            pAudioClient->Release();
                            durationText = fmt::format("minimum {}, default {}", minBufferDuration,
                                                       defaultBufferDuration);
                            entryPtr->minimumBufferDuration = minBufferDuration;
                            entryPtr->defaultBufferDuration = defaultBufferDuration;
                        }
                    } else {
                        durationText = "no latency information provided";
                        SetDlgItemTextW(hWnd, IDC_DEVICE_FRIENDLY_NAME, L"(unknown device)");
                        entryPtr->minimumBufferDuration = 0;
                        entryPtr->defaultBufferDuration = 0;
                    }

                    SetDlgItemTextA(hWnd, IDC_DEVICE_DURATION, durationText.c_str());
                    return TRUE;
                }

                case IDB_USE_MINIMUM_DURATION: {
                    auto entryPtr = (OutputLatencyEntry *) GetWindowLongPtr(hWnd, GWLP_USERDATA);
                    SetDlgItemInt(hWnd, IDC_EDIT_DURATION, entryPtr->minimumBufferDuration, TRUE);
                    break;
                }

                case IDB_USE_DEFAULT_DURATION: {
                    auto entryPtr = (OutputLatencyEntry *) GetWindowLongPtr(hWnd, GWLP_USERDATA);
                    auto duration = (int) GetDlgItemInt(hWnd, IDC_EDIT_DURATION, nullptr, TRUE);
                    SetDlgItemInt(hWnd, IDC_EDIT_DURATION, entryPtr->defaultBufferDuration, TRUE);
                    break;
                }

                case IDC_EDIT_DURATION: {
                    auto duration = (int) GetDlgItemInt(hWnd, IDC_EDIT_DURATION, nullptr, TRUE);
                    auto message = fmt::format("= {:.1f}ms", duration / 10000.0f);
                    SetDlgItemTextA(hWnd, IDC_DURATION_IN_MS, message.c_str());
                    return TRUE;
                }

                case IDOK: {
                    auto entryPtr = (OutputLatencyEntry *) GetWindowLongPtr(hWnd, GWLP_USERDATA);

                    auto hDevice = GetDlgItem(hWnd, IDC_DEVICE);
                    entryPtr->deviceId = getWndText(hDevice);
                    entryPtr->duration = (int) GetDlgItemInt(hWnd, IDC_EDIT_DURATION, nullptr, TRUE);
                    entryPtr->deviceList.clear();

                    EndDialog(hWnd, TRUE);
                    return TRUE;
                }

                case IDCANCEL: {
                    auto entryPtr = (OutputLatencyEntry *) GetWindowLongPtr(hWnd, GWLP_USERDATA);
                    entryPtr->deviceList.clear();
                    EndDialog(hWnd, FALSE);
                    return TRUE;
                }

                default:
                    return FALSE;
            }
        }

        default:
            return FALSE;
    }
}


static BOOL PromptOutputOverrideEntry(HINSTANCE hInstance, HWND hWndParent, std::wstring *deviceName, int *duration) {
    OutputLatencyEntry entry{*deviceName, *duration};
    auto ret = DialogBoxParam(
            hInstance,
            MAKEINTRESOURCE(IDD_OUTPUT_LATENCY_OVERRIDE_ENTRY_EDIT),
            hWndParent,
            DlgPromptOutputOverrideEntry,
            (LPARAM) &entry);

    if (ret) {
        *deviceName = entry.deviceId;
        *duration = entry.duration;
    }

    return ret;
}

////////////////////

static INT_PTR CALLBACK DlgUserPrefDurationOverrideEditProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_INITDIALOG: {
            auto prefPtr = reinterpret_cast<UserPrefPtr *>(lParam);
            SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR) prefPtr);

            auto hTable = GetDlgItem(hWnd, IDC_OUTPUT_LATENCY_TABLE);
            auto &pref = *prefPtr;
            for (const auto &it: pref->durationOverride) {
                auto ws = fmt::format(L"{}: {}", it.first, it.second);
                SendMessageW(hTable, LB_ADDSTRING, 0, (LPARAM) ws.c_str());
            }

            return TRUE;
        }

        case WM_CLOSE:
            EndDialog(hWnd, FALSE);
            return TRUE;

        case WM_COMMAND: {
            WORD id = LOWORD(wParam);
            switch (id) {
                case IDCANCEL:
                    EndDialog(hWnd, FALSE);
                    return TRUE;

                case IDOK: {
                    auto prefPtr = reinterpret_cast<UserPrefPtr *>(GetWindowLongPtr(hWnd, GWLP_USERDATA));
                    auto &pref = *prefPtr;
                    auto hTable = GetDlgItem(hWnd, IDC_OUTPUT_LATENCY_TABLE);
                    auto hTableCount = ListBox_GetCount(hTable);

                    std::map<std::wstring, int> table;
                    std::wstring deviceId;
                    int duration;

                    for (int i = 0; i < hTableCount; i++) {
                        auto ws = ListBox_GetWString(hTable, i);
                        if (parseDurationOverrideLine(ws, &deviceId, &duration)) {
                            table[deviceId] = duration;
                        }
                    }

                    pref->durationOverride = table;
                    EndDialog(hWnd, TRUE);
                    return TRUE;
                }

                case IDB_ADD: {
                    auto hInstance = (HINSTANCE) GetWindowLongPtr(hWnd, GWLP_HINSTANCE);
                    auto hTable = GetDlgItem(hWnd, IDC_OUTPUT_LATENCY_TABLE);
                    std::wstring deviceName;
                    int duration = 0;

                    if (PromptOutputOverrideEntry(hInstance, hWnd, &deviceName, &duration)) {
                        auto ws = fmt::format(L"{}: {}", deviceName, duration);
                        SendMessageW(hTable, LB_ADDSTRING, 0,
                                     (LPARAM) ws.c_str());
                    }
                    break;
                }

                case IDB_REMOVE: {
                    auto hTable = GetDlgItem(hWnd, IDC_OUTPUT_LATENCY_TABLE);
                    auto curSel = ListBox_GetCurSel(hTable);
                    if (curSel != LB_ERR) {
                        SendMessageW(hTable, LB_DELETESTRING, curSel, 0);
                    }
                    break;
                }

                case IDC_OUTPUT_LATENCY_TABLE: {
                    auto hTable = GetDlgItem(hWnd, IDC_OUTPUT_LATENCY_TABLE);
                    switch (HIWORD(wParam)) {
                        case LBN_DBLCLK: {
                            auto curSel = ListBox_GetCurSel(hTable);
                            if (curSel != LB_ERR) {
                                auto hInstance = (HINSTANCE) GetWindowLongPtr(hWnd, GWLP_HINSTANCE);
                                auto line = ListBox_GetWString(hTable, curSel);
                                std::wstring deviceName;
                                int duration;
                                if (parseDurationOverrideLine(ListBox_GetWString(hTable, curSel), &deviceName,
                                                              &duration)) {
                                    if (PromptOutputOverrideEntry(hInstance, hWnd, &deviceName, &duration)) {
                                        auto ws = fmt::format(L"{}: {}", deviceName, duration);
                                        SendMessageW(hTable, LB_DELETESTRING, curSel, 0);
                                        SendMessageW(hTable, LB_INSERTSTRING, curSel,
                                                     (LPARAM) ws.c_str());
                                    }

                                }
                            }
                        }
                    }
                    break;

                }
                default:
                    return FALSE;
            }
        }

        default:
            return FALSE;
    }
}

BOOL DialogDurationOverrideEditor(HINSTANCE hInstance, HWND hWndParent, UserPrefPtr pref) {
    return DialogBoxParam(hInstance, MAKEINTRESOURCE(IDD_OUTPUT_LATENCY_OVERRIDE_EDIT), hWndParent,
                          DlgUserPrefDurationOverrideEditProc, (LPARAM) &pref);
}

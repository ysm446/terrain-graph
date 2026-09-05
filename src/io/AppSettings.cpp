#include "io/AppSettings.h"

#include "core/Log.h"

#include <nlohmann/json.hpp>

#include <Windows.h>

#include <algorithm>
#include <fstream>
#include <string>
#include <system_error>

namespace tg::io {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;

constexpr const char* kFormat = "terrain-graph.settings";
constexpr int kVersion = 1;

fs::path SettingsPath() {
    return AppDataDirectory() / L"settings.json";
}

}  // namespace

fs::path AppDataDirectory() {
    const DWORD needed = ::GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (needed > 0) {
        std::wstring value;
        value.resize(needed);
        const DWORD written = ::GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), needed);
        if (written > 0) {
            value.resize(written);
            return fs::path(value) / L"terrain-graph";
        }
    }
    return fs::path(L".");
}

void AppSettings::Load() {
    m_ui = UiSettings{};
    m_display = DisplaySettings{};

    std::ifstream stream(SettingsPath(), std::ios::binary);
    if (!stream.is_open()) {
        return;  // まだ設定が無いだけ。既定値で始める。
    }

    // 例外は使わない方針なので、パース失敗は discarded で受ける。
    const json document = json::parse(stream, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        TG_LOG_WARN("設定を読めませんでした。既定値で始めます");
        return;
    }
    const auto format = document.find("format");
    if (format == document.end() || !format->is_string() ||
        format->get<std::string>() != kFormat) {
        return;
    }

    if (const auto ui = document.find("ui"); ui != document.end() && ui->is_object()) {
        if (const auto follow = ui->find("followSystemScale");
            follow != ui->end() && follow->is_boolean()) {
            m_ui.followSystemScale = follow->get<bool>();
        }
        if (const auto scale = ui->find("manualScale");
            scale != ui->end() && scale->is_number()) {
            // 壊れた値でも操作不能にならないよう、範囲へ丸める。
            m_ui.manualScale = std::clamp(scale->get<float>(), 0.5f, 4.0f);
        }
        if (const auto fontSize = ui->find("fontSize");
            fontSize != ui->end() && fontSize->is_number_integer()) {
            // ImGuiLayer 側でも切るが、UI のスライダーに載る値にしておく。
            m_ui.fontSize = std::clamp(fontSize->get<int>(), 11, 28);
        }
        if (const auto listHeight = ui->find("layerListHeight");
            listHeight != ui->end() && listHeight->is_number()) {
            m_ui.layerListHeight = std::clamp(listHeight->get<float>(), 100.0f, 800.0f);
        }
    }

    if (const auto display = document.find("display");
        display != document.end() && display->is_object()) {
        if (const auto vsync = display->find("vsync");
            vsync != display->end() && vsync->is_boolean()) {
            m_display.vsync = vsync->get<bool>();
        }
        if (const auto hotReload = display->find("hotReload");
            hotReload != display->end() && hotReload->is_boolean()) {
            m_display.hotReload = hotReload->get<bool>();
        }
        if (const auto showFps = display->find("showFps");
            showFps != display->end() && showFps->is_boolean()) {
            m_display.showFps = showFps->get<bool>();
        }
        if (const auto showStats = display->find("showStats");
            showStats != display->end() && showStats->is_boolean()) {
            m_display.showStats = showStats->get<bool>();
        }
        if (const auto showHeightGuide = display->find("showHeightGuide");
            showHeightGuide != display->end() && showHeightGuide->is_boolean()) {
            m_display.showHeightGuide = showHeightGuide->get<bool>();
        }
        if (const auto showAssetBand = display->find("showAssetBand");
            showAssetBand != display->end() && showAssetBand->is_boolean()) {
            m_display.showAssetBand = showAssetBand->get<bool>();
        }
        // 壊れた値でも操作不能にならないよう、範囲へ丸める（0 は上限なし）。
        if (const auto limit = display->find("frameRateLimit");
            limit != display->end() && limit->is_number_integer()) {
            m_display.frameRateLimit = std::clamp(limit->get<int>(), 0, 480);
        }
        if (const auto limit = display->find("inactiveFrameRateLimit");
            limit != display->end() && limit->is_number_integer()) {
            m_display.inactiveFrameRateLimit = std::clamp(limit->get<int>(), 0, 480);
        }
        if (const auto color = display->find("clearColor");
            color != display->end() && color->is_array() && color->size() >= 3) {
            for (size_t i = 0; i < 3; ++i) {
                if ((*color)[i].is_number()) {
                    m_display.clearColor[i] =
                        std::clamp((*color)[i].get<float>(), 0.0f, 1.0f);
                }
            }
        }
    }
}

bool AppSettings::Save() const {
    const fs::path path = SettingsPath();
    std::error_code error;
    if (const fs::path parent = path.parent_path(); !parent.empty()) {
        fs::create_directories(parent, error);
    }

    json ui;
    ui["followSystemScale"] = m_ui.followSystemScale;
    ui["manualScale"] = m_ui.manualScale;
    ui["fontSize"] = m_ui.fontSize;
    ui["layerListHeight"] = m_ui.layerListHeight;

    json display;
    display["vsync"] = m_display.vsync;
    display["hotReload"] = m_display.hotReload;
    display["showFps"] = m_display.showFps;
    display["showStats"] = m_display.showStats;
    display["showHeightGuide"] = m_display.showHeightGuide;
    display["showAssetBand"] = m_display.showAssetBand;
    display["frameRateLimit"] = m_display.frameRateLimit;
    display["inactiveFrameRateLimit"] = m_display.inactiveFrameRateLimit;
    display["clearColor"] = {m_display.clearColor[0], m_display.clearColor[1],
                             m_display.clearColor[2]};

    json document;
    document["format"] = kFormat;
    document["version"] = kVersion;
    document["ui"] = std::move(ui);
    document["display"] = std::move(display);

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
        TG_LOG_WARN("設定を保存できませんでした");
        return false;
    }
    // 壊れた文字列が混ざっていても例外を出さない（不正な UTF-8 は置換文字にする）。
    stream << document.dump(2, ' ', false, json::error_handler_t::replace) << '\n';
    return stream.good();
}

}  // namespace tg::io

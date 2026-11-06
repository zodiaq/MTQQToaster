#pragma once

#include "app_logic.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct SettingsUiCallbacks {
    std::function<AppConfig()> loadConfig;
    std::function<bool(const AppConfig&, const std::vector<std::filesystem::path>&, std::wstring&)> saveConfig;
    std::function<std::optional<SettingsWindowPlacement>()> loadPlacement;
    std::function<void(const SettingsWindowPlacement&)> savePlacement;
};

class SettingsUi {
public:
    explicit SettingsUi(SettingsUiCallbacks callbacks);
    ~SettingsUi();

    SettingsUi(const SettingsUi&) = delete;
    SettingsUi& operator=(const SettingsUi&) = delete;

    void ShowSettings(bool resetPlacement = false);
    bool IsSettingsReady() const;
    bool IsDefaultLayoutReady() const;
    HWND SettingsHandle() const;
    void CloseAll();
    bool ProcessMessage(MSG& message);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

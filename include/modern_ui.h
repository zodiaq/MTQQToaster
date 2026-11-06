#pragma once

#include "app_logic.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct ModernUiCallbacks {
    std::function<AppConfig()> loadConfig;
    std::function<bool(const AppConfig&, const std::vector<std::filesystem::path>&, std::wstring&)> saveConfig;
    std::function<std::optional<SettingsWindowPlacement>()> loadPlacement;
    std::function<void(const SettingsWindowPlacement&)> savePlacement;
};

class ModernUi {
public:
    explicit ModernUi(ModernUiCallbacks callbacks);
    ~ModernUi();

    ModernUi(const ModernUi&) = delete;
    ModernUi& operator=(const ModernUi&) = delete;

    void ShowSettings(bool resetPlacement = false);
    bool IsSettingsReady() const;
    bool IsDefaultLayoutReady() const;
    HWND SettingsHandle() const;
    void CloseAll();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

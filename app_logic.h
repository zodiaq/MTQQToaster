#pragma once

#include <string>
#include <filesystem>
#include <windows.h>
#include <spdlog/spdlog.h>
#include "resource.h"

#define MQTTTOASTER_DEFAULT_TITLE "MQTT Toaster"

// 通知用データ構造体
struct NotificationInfo {
    std::string title = MQTTTOASTER_DEFAULT_TITLE;
    std::string message;
    std::string imageUrl;
};

// 設定用構造体
struct AppConfig {
    std::string url = "mqtt://127.0.0.1:1883";
    std::vector<std::string> topics = {"mqtt_toaster/#"};
    std::string user = "";
    std::string pass = "";
    std::string logLevel = "INFO";
    size_t maxLogSizeMb = 5;
    size_t maxLogFiles = 3;
};

// --- テスト・利用対象関数 ---
std::filesystem::path GetModuleDirectory();

NotificationInfo ParseNotificationPayload(const std::string& payload);

void ReadLogConfigFirst(const std::filesystem::path& configPath, size_t& outSizeMb, size_t& outMaxFiles);
void InitLogger(const std::filesystem::path& logPath, size_t maxMb, size_t maxFiles);
void UpdateLogLevel(const std::string& levelStr);
bool LoadConfig(const std::filesystem::path& configPath, AppConfig& outConfig);

void ShowToast(const NotificationInfo& data);

// タスクトレイ管理クラス
class TrayIcon {
public:
    HWND hwnd = NULL;
    NOTIFYICONDATAW nid = { 0 };

    bool Create(HINSTANCE hInstance);
    void Destroy();
};
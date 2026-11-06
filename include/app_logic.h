#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <functional>
#include <windows.h>
#include <spdlog/spdlog.h>

#define MQTTTOASTER_DEFAULT_TITLE "MQTT Toaster"

// 通知用データ構造体
struct NotificationInfo {
    std::string title = MQTTTOASTER_DEFAULT_TITLE;
    std::string message;
    std::string imageUrl;
};

struct TopicConfig {
    std::string topic;
    std::string image;
};

// 設定用構造体
struct AppConfig {
    std::string url = "mqtt://127.0.0.1:1883";
    std::vector<TopicConfig> topics = {{"mqtt_toaster/#", ""}};
    std::string user = "";
    std::string pass = "";
    std::string logLevel = "INFO";
    size_t maxLogSizeMb = 5;
    size_t maxLogFiles = 3;
};

struct AppStoragePaths {
    std::filesystem::path configDirectory;
    std::filesystem::path cacheDirectory;
    std::filesystem::path toastAssetsDirectory;
    std::filesystem::path configPath;
    std::filesystem::path logPath;
};

// --- テスト・利用対象関数 ---
std::filesystem::path GetModuleDirectory();
AppStoragePaths GetAppDataPaths();
bool InitializeAppData(const AppStoragePaths& paths);

NotificationInfo ParseNotificationPayload(const std::string& payload);

void ReadLogConfigFirst(const std::filesystem::path& configPath, size_t& outSizeMb, size_t& outMaxFiles);
void InitLogger(const std::filesystem::path& logPath, size_t maxMb, size_t maxFiles);
void UpdateLogLevel(const std::string& levelStr);
bool LoadConfig(const std::filesystem::path& configPath, AppConfig& outConfig);
bool SaveConfig(const std::filesystem::path& configPath, const AppConfig& config);
bool CopyToastImage(const std::filesystem::path& configDirectory,
                    const std::filesystem::path& sourcePath,
                    std::string& outFileName);
void RemoveUnusedToastImages(const std::filesystem::path& configDirectory,
                             const std::vector<TopicConfig>& topics);

void ShowToast(const NotificationInfo& data);

// タスクトレイ管理クラス
class TrayIcon {
public:
    HWND hwnd = NULL;
    NOTIFYICONDATAW nid = { 0 };

    bool Create(HINSTANCE hInstance, std::function<void(HWND)> settingsHandler);
    void Destroy();
    void OpenSettings(HWND owner) const;

private:
    std::function<void(HWND)> settingsHandler_;
};

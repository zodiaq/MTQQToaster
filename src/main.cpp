#include "app_logic.h"
#include <thread>
#include <atomic>
#include <filesystem>
#include <mosquitto.h>
#include <fmt/ranges.h> // fmt::join に必要

#include <winrt/Windows.Foundation.h>
#include <winrt/windows.foundation.collections.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Search.h>

using namespace winrt;
using namespace Windows::Storage;
using namespace Windows::Storage::Search;

AppConfig g_config;
SRWLOCK g_configLock = SRWLOCK_INIT;
std::atomic<bool> g_needsReconnect{ false };
std::filesystem::file_time_type g_lastWriteTime{};

// イベントトークンを保持して監視を継続
event_token g_contentsChangedToken;
StorageFileQueryResult g_queryResult{ nullptr };

Windows::Foundation::IAsyncAction StartConfigWatcherAsync(std::wstring folderPath, std::wstring fileName)
{
    try {
        // 1. フォルダとファイルの取得
        const StorageFolder folder = co_await StorageFolder::GetFolderFromPathAsync(folderPath);

        // 2. 特定ファイルのみを対象にするクエリ条件を作成
        const QueryOptions options(
            CommonFileQuery::DefaultQuery,
            winrt::single_threaded_vector<winrt::hstring>({ L".json" })
        );
        // ディスクの直接変更を検知するように設定
        options.IndexerOption(Windows::Storage::Search::IndexerOption::DoNotUseIndexer);

        g_queryResult = folder.CreateFileQueryWithOptions(options);
        const auto files = co_await g_queryResult.GetFilesAsync();


        // 3. ファイル変更イベントの購読
        g_contentsChangedToken = g_queryResult.ContentsChanged([folderPath, fileName](auto const& /*sender*/, auto const& /*args*/) {
            std::error_code ec;
			auto configFilePath = std::filesystem::path(folderPath) / fileName;
            if (!std::filesystem::exists(configFilePath, ec)) {
                return;
            }

            // 実際のファイル更新日時を取得して比較
            auto currentWriteTime = std::filesystem::last_write_time(configFilePath, ec);
            if (ec || currentWriteTime == g_lastWriteTime) {
                // タイムスタンプが変わっていない（別ファイル変更やメタデータのみの通知）場合は無視
                return;
            }

            // エディタのファイル書き込み完了を少し待機
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            // 更新日時を再取得して記録
            currentWriteTime = std::filesystem::last_write_time(configFilePath, ec);
            g_lastWriteTime = currentWriteTime;

            spdlog::info("Config file changed: {}", winrt::to_string(fileName));

            AppConfig newCfg;
            if (LoadConfig(configFilePath, newCfg)) {
                // g_configLock による RAII 排他制御
                {
                    AcquireSRWLockExclusive(&g_configLock);
                    g_config = std::move(newCfg);
                    ReleaseSRWLockExclusive(&g_configLock);
                }
                g_needsReconnect.store(true, std::memory_order_release);
                spdlog::info("Config reloaded successfully.");
            }
            else {
                spdlog::warn("Failed to reload config.");
            }
            });

        spdlog::info("Watcher initialized for: {}", winrt::to_string(fileName));
    }
    catch (hresult_error const& ex) {
        spdlog::error("WinRT Watcher Error: {}", winrt::to_string(ex.message()));
    }
}

// 監視停止処理
void StopConfigWatcher()
{
    if (g_queryResult && g_contentsChangedToken) {
        g_queryResult.ContentsChanged(g_contentsChangedToken);
        g_contentsChangedToken = {};
        g_queryResult = nullptr;
    }
}

void on_message(struct mosquitto* mosq, void* obj, const struct mosquitto_message* msg) {
    if (msg->payloadlen <= 0) return;
    
    const std::string payload(static_cast<char*>(msg->payload), msg->payloadlen);
    spdlog::trace("MQTT payload received: {}", payload);

    const NotificationInfo data = ParseNotificationPayload(payload);
    ShowToast(data);
}

void MqttWorkerThread(std::stop_token stoken) {
    mosquitto_lib_init();

    while (!stoken.stop_requested()) {
        struct mosquitto* mosq = mosquitto_new("mqtt_toaster", true, NULL);
        if (!mosq) break;

        AcquireSRWLockShared(&g_configLock);
        const AppConfig cfg = g_config;
        ReleaseSRWLockShared(&g_configLock);

        if (!cfg.user.empty() && !cfg.pass.empty()) {
            mosquitto_username_pw_set(mosq, cfg.user.c_str(), cfg.pass.c_str());
        }

        mosquitto_message_callback_set(mosq, on_message);
        spdlog::info("Connecting to MQTT Broker {}...", cfg.url);

        // Parse URL to extract host and port
        Windows::Foundation::Uri uri(winrt::to_hstring(cfg.url));
        const std::string protocol = winrt::to_string(uri.SchemeName()); // e.g. L"mqtts"
        const std::string host = winrt::to_string(uri.Host()); // e.g. L"192.168.16.25"
        int port = uri.Port(); // e.g. 8883

		if (protocol == "mqtts") {
            mosquitto_int_option(mosq, MOSQ_OPT_TLS_USE_OS_CERTS, 1);
            mosquitto_tls_set(mosq, "", NULL, NULL, NULL, NULL);
            mosquitto_tls_insecure_set(mosq, true);
			spdlog::info("Using TLS for MQTT connection.");
		}

        if (mosquitto_connect(mosq, host.c_str(), port, 60) == MOSQ_ERR_SUCCESS) {
            spdlog::info("Connected to MQTT Broker. Subscribing to topics: {}", fmt::join(cfg.topics, ", "));
            for (const auto& topic : cfg.topics) {
                mosquitto_subscribe(mosq, NULL, topic.c_str(), 0);
            }
            g_needsReconnect = false;

            while (!stoken.stop_requested() && !g_needsReconnect) {
                int rc = mosquitto_loop(mosq, 1000, 1);
                if (rc != MOSQ_ERR_SUCCESS) {
                    Sleep(2000);
                    break;
                }
            }
            mosquitto_disconnect(mosq);
        }
        else {
            for (int i = 0; i < 5 && !stoken.stop_requested(); ++i) Sleep(1000);
        }
        mosquitto_destroy(mosq);
    }
    mosquitto_lib_cleanup();
}

int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR lpCmdLine, _In_ int nCmdShow) {
    init_apartment(apartment_type::single_threaded);

	// preventing multiple instances
	HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Global\\MQTTToasterMutex");
	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		MessageBoxW(NULL, L"MQTT Toaster is already running.", L"MQTT Toaster", MB_ICONEXCLAMATION | MB_OK);
		return 0;
	}

    const auto exeDir = GetModuleDirectory();
    const auto logPath = exeDir / L"app.log";
    const std::wstring configFileName = L"config.json";
    const auto configPath = exeDir / configFileName;

    size_t initLogSizeMb = 5, initLogMaxFiles = 3;
    ReadLogConfigFirst(configPath, initLogSizeMb, initLogMaxFiles);
    InitLogger(logPath, initLogSizeMb, initLogMaxFiles);

    spdlog::info("=== Application Started ===");
    LoadConfig(configPath, g_config);

    TrayIcon tray;
    if (!tray.Create(hInstance)) spdlog::error("Failed to create tray icon.");

	// start WinRT-based config watcher
	StartConfigWatcherAsync(exeDir, configFileName);

    std::jthread mqttThread(MqttWorkerThread);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    mqttThread.request_stop(); mqttThread.join();

	// Stop WinRT-based config watcher
	StopConfigWatcher();

    tray.Destroy();
    spdlog::info("=== Application Terminating ===");
    spdlog::shutdown();

    uninit_apartment();
    return 0;
}
#include "app_logic.h"
#include "settings_ui.h"
#include "resource.h"
#include <thread>
#include <atomic>
#include <filesystem>
#include <string_view>
#include <chrono>
#include <stdexcept>
#include <commctrl.h>
#include "mqtt_client.h"

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
AppStoragePaths g_appDataPaths;

namespace {

struct ConfigLock {
    explicit ConfigLock(bool exclusive) : exclusive_(exclusive) {
        if (exclusive_) AcquireSRWLockExclusive(&g_configLock);
        else AcquireSRWLockShared(&g_configLock);
    }
    ~ConfigLock() {
        if (exclusive_) ReleaseSRWLockExclusive(&g_configLock);
        else ReleaseSRWLockShared(&g_configLock);
    }
    ConfigLock(const ConfigLock&) = delete;
    ConfigLock& operator=(const ConfigLock&) = delete;
    bool exclusive_;
};

AppConfig GetConfigSnapshot()
{
    AppConfig config;
    const ConfigLock lock(false);
    config = g_config;
    return config;
}

bool SaveSettings(const AppConfig& requestedConfig,
                        const std::vector<std::filesystem::path>& selectedImages,
                        std::wstring& error)
{
    AppConfig config = requestedConfig;
    try { ValidateMqttConfig(config); }
    catch (const std::exception& ex) {
        error = winrt::to_hstring(ex.what());
        return false;
    }
    if (config.topics.empty()) {
        error = L"トピックを1件以上入力してください。";
        return false;
    }
    if (selectedImages.size() != config.topics.size()) {
        error = L"topicの画像設定が不整合です。設定画面を開き直してください。";
        return false;
    }

    const AppConfig activeConfig = GetConfigSnapshot();
    for (size_t i = 0; i < selectedImages.size(); ++i) {
        if (selectedImages[i].empty()) continue;
        std::string copiedFile;
        if (!CopyToastImage(g_appDataPaths.configDirectory, selectedImages[i], copiedFile)) {
            RemoveUnusedToastImages(g_appDataPaths.configDirectory, activeConfig.topics);
            error = L"画像を設定フォルダーへコピーできませんでした。PNG、JPEG、GIFを選択してください。";
            return false;
        }
        config.topics[i].image = copiedFile;
    }

    if (!SaveConfig(g_appDataPaths.configPath, config)) {
        RemoveUnusedToastImages(g_appDataPaths.configDirectory, activeConfig.topics);
        error = L"設定を保存できませんでした。";
        return false;
    }

    RemoveUnusedToastImages(g_appDataPaths.configDirectory, config.topics);
    {
        const ConfigLock lock(true);
        g_config = config;
    }
    UpdateLogLevel(config.logLevel);
    g_needsReconnect.store(true, std::memory_order_release);
    spdlog::info("Settings saved from the Win32 window.");
    return true;
}

}

constexpr UINT WM_CONFIG_CHANGED = WM_APP + 2;
std::atomic<bool> g_watcherStopping{false};

Windows::Foundation::IAsyncAction StartConfigWatcherAsync(std::wstring folderPath, DWORD ownerThread)
{
    try {
        const StorageFolder folder = co_await StorageFolder::GetFolderFromPathAsync(folderPath);
        if (g_watcherStopping.load()) co_return;
        const QueryOptions options(CommonFileQuery::DefaultQuery,
            winrt::single_threaded_vector<winrt::hstring>({L".json"}));
        options.IndexerOption(Windows::Storage::Search::IndexerOption::DoNotUseIndexer);
        auto query = folder.CreateFileQueryWithOptions(options);
        co_await query.GetFilesAsync();
        if (g_watcherStopping.load()) co_return;
        // The coroutine resumes on the owning STA. Event callbacks only queue work to that STA.
        g_queryResult = query;
        g_contentsChangedToken = query.ContentsChanged([ownerThread](auto&&, auto&&) noexcept {
            if (!g_watcherStopping.load()) PostThreadMessageW(ownerThread, WM_CONFIG_CHANGED, 0, 0);
        });
        spdlog::info("Configuration watcher initialized.");
    }
    catch (const hresult_canceled&) {}
    catch (const hresult_error& ex) {
        spdlog::error("WinRT Watcher Error: {}", winrt::to_string(ex.message()));
    }
}

void ReloadConfig()
{
    std::error_code error;
    const auto timestamp = std::filesystem::last_write_time(g_appDataPaths.configPath, error);
    if (error || timestamp == g_lastWriteTime) return;
    AppConfig config;
    if (!LoadConfig(g_appDataPaths.configPath, config)) return;
    {
        const ConfigLock lock(true);
        g_config = std::move(config);
    }
    g_lastWriteTime = timestamp;
    g_needsReconnect.store(true, std::memory_order_release);
    UpdateLogLevel(GetConfigSnapshot().logLevel);
    spdlog::info("Configuration reloaded.");
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

void MqttWorkerThread(std::stop_token stop) noexcept
{
    try {
        init_apartment(apartment_type::multi_threaded);
        struct ApartmentScope { ~ApartmentScope() { uninit_apartment(); } } apartmentScope;
        const std::string clientId = fmt::format("mtqq{:08x}{:08x}", GetCurrentProcessId(), static_cast<uint32_t>(GetTickCount64()));
        while (!stop.stop_requested()) {
            // Clear before taking the snapshot: a concurrent update remains visible to cancellation.
            g_needsReconnect.exchange(false, std::memory_order_acq_rel);
            const AppConfig config = GetConfigSnapshot();
            const MqttCancelled cancelled = [&] {
                return stop.stop_requested() || g_needsReconnect.load(std::memory_order_acquire);
            };
            try {
                ValidateMqttConfig(config);
                WindowsMqttTransport transport(cancelled);
                const auto endpoint = ParseMqttEndpoint(config.url);
                spdlog::info("Connecting to MQTT broker (TLS: {}).", endpoint.tls);
                transport.Connect(endpoint);
                try {
                    RunMqttSession(config, transport, cancelled,
                        [&config](std::string_view topic, std::string_view payload) {
                            auto notification = ParseNotificationPayload(std::string(payload));
                            if (notification.imageUrl.empty()) {
                                for (const auto& subscription : config.topics) {
                                    if (MqttTopicMatches(subscription.topic, topic)) {
                                        notification.imageUrl = subscription.image;
                                        break;
                                    }
                                }
                            }
                            ShowToast(notification);
                        }, MqttSessionOptions{60, 5000, clientId});
                }
                catch (...) {
                    if (!cancelled() && !transport.LastError().empty()) spdlog::warn("{}", transport.LastError());
                    throw;
                }
            }
            catch (const std::exception& ex) {
                if (!cancelled()) spdlog::warn("MQTT connection ended: {}", ex.what());
            }
            catch (...) {
                if (!cancelled()) spdlog::warn("MQTT connection ended with a Windows API error.");
            }
            // Cancellation remains responsive during reconnect backoff.
            for (int i = 0; i < 50 && !cancelled(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    catch (...) { OutputDebugStringW(L"MTQQToaster: MQTT worker initialization failed.\n"); }
}

namespace {

std::unique_ptr<SettingsUi> g_settingsUi;
TrayIcon g_tray;
std::unique_ptr<std::jthread> g_mqttThread;
HINSTANCE g_instance = nullptr;
bool g_uiSmokeTest = false;
bool g_uiPreview = false;
int g_exitCode = 0;

struct App {
    void Launch()
    {
        SettingsUiCallbacks callbacks;
        if (g_uiSmokeTest) {
            smokeConfig.url = "mqtt://localhost:1883";
            smokeConfig.topics = {{"test/日本語", "old-image.png"}};
            callbacks.loadConfig = [this] { return smokeConfig; };
            callbacks.saveConfig = [this](const AppConfig& config, const auto& images, std::wstring& error) {
                if (images.size() != config.topics.size()) { error = L"画像数が一致しません。"; return false; }
                smokeConfig = config;
                ++smokeSaves;
                return true;
            };
            callbacks.loadPlacement = [] { return LoadSettingsWindowPlacement(GetModuleDirectory() / L"ui-smoke-state.json"); };
            callbacks.savePlacement = [](const auto& placement) {
                if (!SaveSettingsWindowPlacement(GetModuleDirectory() / L"ui-smoke-state.json", placement)) {
                    throw std::runtime_error("Could not save UI smoke placement");
                }
            };
        }
        else {
            callbacks.loadConfig = GetConfigSnapshot;
            callbacks.saveConfig = SaveSettings;
            const auto placementPath = g_appDataPaths.configDirectory / L"ui-state.json";
            callbacks.loadPlacement = [placementPath] { return LoadSettingsWindowPlacement(placementPath); };
            callbacks.savePlacement = [placementPath](const auto& placement) {
                if (!SaveSettingsWindowPlacement(placementPath, placement)) {
                    spdlog::warn("Could not save settings window placement.");
                }
            };
        }
        g_settingsUi = std::make_unique<SettingsUi>(std::move(callbacks));

        if (g_uiSmokeTest) {
            if (g_uiPreview) g_settingsUi->ShowSettings(true);
            else StartUiSmokeTest();
            return;
        }

        if (!g_tray.Create(
                g_instance,
                [] { if (g_settingsUi) g_settingsUi->ShowSettings((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0); },
                [] {
                    spdlog::info("Exit selected from tray menu. Terminating...");
                    PostQuitMessage(0);
                })) {
            throw std::runtime_error("Failed to create tray icon");
        }

        watcher = StartConfigWatcherAsync(g_appDataPaths.configDirectory.wstring(), GetCurrentThreadId());
        g_mqttThread = std::make_unique<std::jthread>(MqttWorkerThread);
        spdlog::info("Tray application ready.");
    }

    void StartUiSmokeTest()
    {
        smokeTimer = SetTimer(nullptr, 0, 100, nullptr);
        check_bool(smokeTimer != 0);
        spdlog::info("UI smoke test: waiting for Win32 message loop.");
    }

    bool IsSmokeTimer(const MSG& message) const
    {
        return smokeTimer && message.message == WM_TIMER && message.hwnd == nullptr && message.wParam == smokeTimer;
    }

    void Tick()
    {
        try {
            switch (smokeStep++) {
            case 0:
                spdlog::info("UI smoke test: opening settings after windowless startup.");
                g_settingsUi->ShowSettings(true);
                break;
            case 1:
                if (!g_settingsUi->IsDefaultLayoutReady()) throw std::runtime_error("Default settings layout is incorrect");
                {
                    const auto hwnd = g_settingsUi->SettingsHandle();
                    check_bool(GetWindowRect(hwnd, &smokeBounds));
                    check_bool(SetWindowPos(hwnd, nullptr, smokeBounds.left + 12, smokeBounds.top + 12,
                        smokeBounds.right - smokeBounds.left - 40, smokeBounds.bottom - smokeBounds.top - 40,
                        SWP_NOZORDER | SWP_NOACTIVATE));
                    check_bool(GetWindowRect(hwnd, &smokeBounds));
                }
                break;
            case 2:
                g_settingsUi->CloseAll();
                break;
            case 3:
                g_settingsUi->ShowSettings();
                break;
            case 4:
                if (!g_settingsUi->IsSettingsReady()) throw std::runtime_error("Settings controls did not reload");
                {
                    RECT restored{};
                    check_bool(GetWindowRect(g_settingsUi->SettingsHandle(), &restored));
                    if (!EqualRect(&smokeBounds, &restored)) throw std::runtime_error("Window placement was not restored");
                }
                spdlog::info("UI smoke test: saved position and size restored.");
                g_settingsUi->ShowSettings(true);
                break;
            case 5:
                if (!g_settingsUi->IsDefaultLayoutReady()) throw std::runtime_error("Window placement reset failed");
                spdlog::info("UI smoke test: default placement reset passed.");
                g_settingsUi->CloseAll();
                break;
            case 6:
                // Exercise real dialog commands against an in-memory configuration.
                g_settingsUi->ShowSettings(true);
                {
                    const auto hwnd = g_settingsUi->SettingsHandle();
                    const auto form = GetDlgItem(hwnd, IDC_SETTINGS_FORM);
                    check_pointer(form);
                    check_bool(SetDlgItemTextW(form, IDC_TOPIC_IMAGE, L""));
                    SendMessageW(form, WM_COMMAND, MAKEWPARAM(IDC_TOPIC_EDIT, BN_CLICKED), 0);
                    SendMessageW(form, WM_COMMAND, MAKEWPARAM(IDC_TOPIC_ADD, BN_CLICKED), 0);
                    check_bool(SetDlgItemTextW(form, IDC_TOPIC_NAME, L"追加/日本語/#"));
                    SendMessageW(form, WM_COMMAND, MAKEWPARAM(IDC_TOPIC_EDIT, BN_CLICKED), 0);
                    SendMessageW(form, WM_COMMAND, MAKEWPARAM(IDC_TOPIC_DELETE, BN_CLICKED), 0);
                    check_bool(SetDlgItemTextW(form, IDC_MQTT_USER, L"テスト"));
                    check_bool(SetDlgItemTextW(form, IDC_LOG_SIZE, L"12"));
                    SendMessageW(hwnd, WM_COMMAND, IDOK, 0);
                }
                if (g_settingsUi->SettingsHandle() || smokeSaves != 1 || smokeConfig.topics.size() != 1 ||
                    !smokeConfig.topics[0].image.empty() || smokeConfig.user != "テスト" || smokeConfig.maxLogSizeMb != 12) {
                    throw std::runtime_error("Settings commands did not save the expected configuration");
                }
                g_settingsUi->ShowSettings();
                {
                    const auto hwnd = g_settingsUi->SettingsHandle();
                    check_bool(SetDlgItemTextW(GetDlgItem(hwnd, IDC_SETTINGS_FORM), IDC_MQTT_USER, L"cancelled"));
                    SendMessageW(hwnd, WM_COMMAND, IDCANCEL, 0);
                }
                if (smokeSaves != 1 || smokeConfig.user != "テスト") throw std::runtime_error("Cancel saved settings");
                spdlog::info("UI smoke test: topic add/update/delete, image removal, save and cancel passed.");
                break;
            case 7:
                smokeDirectory = std::filesystem::temp_directory_path() /
                    (L"MTQQToaster-ui-smoke-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
                check_bool(std::filesystem::create_directory(smokeDirectory));
                g_appDataPaths.configDirectory = smokeDirectory;
                g_appDataPaths.configPath = smokeDirectory / L"config.json";
                if (!SaveConfig(g_appDataPaths.configPath, smokeConfig)) throw std::runtime_error("Cannot create watcher fixture");
                watcher = StartConfigWatcherAsync(smokeDirectory.wstring(), GetCurrentThreadId());
                break;
            case 8:
                if (watcher.Status() == Windows::Foundation::AsyncStatus::Started) {
                    if (++watcherWaits > 100) throw std::runtime_error("Watcher initialization timed out");
                    --smokeStep;
                    break;
                }
                watcher.GetResults();
                if (!g_contentsChangedToken) throw std::runtime_error("Watcher failed to subscribe");
                smokeConfig.url = "mqtt://localhost:1884";
                if (!SaveConfig(g_appDataPaths.configPath, smokeConfig)) throw std::runtime_error("Cannot update watcher fixture");
                watcherWaits = 0;
                break;
            case 9:
                if (GetConfigSnapshot().url != smokeConfig.url) {
                    if (++watcherWaits > 100) throw std::runtime_error("Configuration reload timed out");
                    --smokeStep;
                    break;
                }
                spdlog::info("UI smoke test: configuration watcher reload passed.");
                StopConfigWatcher();
                // Begin another initialization and immediately quit to cover shutdown during an await.
                watcher = StartConfigWatcherAsync(smokeDirectory.wstring(), GetCurrentThreadId());
                StopTimer();
                g_exitCode = 0;
                spdlog::info("UI smoke test passed.");
                PostQuitMessage(0);
                break;
            }
        }
        catch (const hresult_error& ex) {
            spdlog::error("UI smoke test failed. HRESULT: 0x{:08x}, message: {}",
                          static_cast<unsigned int>(ex.code()), to_string(ex.message()));
            StopTimer();
            PostQuitMessage(0);
        }
        catch (const std::exception& ex) {
            spdlog::error("UI smoke test failed: {}", ex.what());
            StopTimer();
            PostQuitMessage(0);
        }
    }

    void StopTimer() { if (smokeTimer) { KillTimer(nullptr, smokeTimer); smokeTimer = 0; } }
    bool ProcessConfigMessage(const MSG& message)
    {
        if (message.message == WM_CONFIG_CHANGED && !message.hwnd) {
            configTimer = SetTimer(nullptr, configTimer, 150, nullptr);
            check_bool(configTimer != 0);
            return true;
        }
        if (configTimer && message.message == WM_TIMER && !message.hwnd && message.wParam == configTimer) {
            KillTimer(nullptr, configTimer);
            configTimer = 0;
            ReloadConfig();
            return true;
        }
        return false;
    }

    ~App()
    {
        StopTimer();
        if (configTimer) KillTimer(nullptr, configTimer);
        g_tray.Destroy();
        if (g_settingsUi) g_settingsUi->CloseAll();
        g_watcherStopping.store(true);
        try {
            StopConfigWatcher();
            if (watcher) {
                watcher.Cancel();
                // Complete STA continuations before COM teardown; no new subscriptions are allowed.
                while (watcher.Status() == Windows::Foundation::AsyncStatus::Started) {
                    MsgWaitForMultipleObjectsEx(0, nullptr, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                    MSG message{};
                    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                        if (message.message == WM_QUIT || message.message == WM_CONFIG_CHANGED) continue;
                        TranslateMessage(&message);
                        DispatchMessageW(&message);
                    }
                }
                watcher.GetResults();
            }
        }
        catch (...) { OutputDebugStringW(L"MTQQToaster: configuration watcher stopped.\n"); }
        if (!smokeDirectory.empty()) {
            std::error_code error;
            std::filesystem::remove(smokeDirectory / L"config.json", error);
            std::filesystem::remove(smokeDirectory, error);
        }
    }

    UINT_PTR smokeTimer = 0;
    UINT_PTR configTimer = 0;
    Windows::Foundation::IAsyncAction watcher{nullptr};
    int smokeStep = 0;
    RECT smokeBounds{};
    AppConfig smokeConfig;
    int smokeSaves = 0;
    std::filesystem::path smokeDirectory;
    int watcherWaits = 0;
};

}

int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPSTR commandLine, _In_ int) {
    g_uiPreview = std::string_view(commandLine) == "--ui-preview";
    g_uiSmokeTest = g_uiPreview || std::string_view(commandLine) == "--ui-smoke-test";
    g_exitCode = g_uiSmokeTest && !g_uiPreview ? 1 : 0;
    init_apartment(apartment_type::single_threaded);
    const INITCOMMONCONTROLSEX commonControls{sizeof(commonControls), ICC_STANDARD_CLASSES};
    if (!InitCommonControlsEx(&commonControls)) { uninit_apartment(); return 1; }

    g_instance = hInstance;

	// preventing multiple instances
	HANDLE hMutex = g_uiSmokeTest ? nullptr : CreateMutexW(NULL, TRUE, L"Global\\MQTTToasterMutex");
	if (!g_uiSmokeTest && GetLastError() == ERROR_ALREADY_EXISTS) {
		MessageBoxW(NULL, L"MQTT Toaster is already running.", L"MQTT Toaster", MB_ICONEXCLAMATION | MB_OK);
        uninit_apartment();
        return 0;
	}

    if (g_uiSmokeTest) {
        InitLogger(GetModuleDirectory() / L"ui-smoke.log", 1, 1);
    }
    else {
        g_appDataPaths = GetAppDataPaths();
        if (!InitializeAppData(g_appDataPaths)) {
            MessageBoxW(NULL, L"アプリケーションデータフォルダーを初期化できませんでした。", L"MTQQToaster", MB_ICONERROR | MB_OK);
            if (hMutex) CloseHandle(hMutex);
            uninit_apartment();
            return 1;
        }

        const auto& configPath = g_appDataPaths.configPath;

        size_t initLogSizeMb = 5, initLogMaxFiles = 3;
        ReadLogConfigFirst(configPath, initLogSizeMb, initLogMaxFiles);
        InitLogger(g_appDataPaths.logPath, initLogSizeMb, initLogMaxFiles);

        spdlog::info("=== Application Started ===");
        if (!LoadConfig(configPath, g_config)) {
            SaveConfig(configPath, g_config);
        }
    }

    try {
        App app;
        app.Launch();
        MSG message{};
        int result = 0;
        while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            if (app.IsSmokeTimer(message)) { app.Tick(); continue; }
            if (app.ProcessConfigMessage(message)) continue;
            if (g_settingsUi && g_settingsUi->ProcessMessage(message)) {
                if (g_uiPreview && !g_settingsUi->SettingsHandle()) break;
                continue;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (g_uiPreview && !g_settingsUi->SettingsHandle()) break;
        }
        if (result == -1) g_exitCode = 1;
    }
    catch (const std::exception& ex) {
        g_exitCode = 1;
        spdlog::error("Application failed: {}", ex.what());
    }
    catch (...) {
        g_exitCode = 1;
        spdlog::error("Application failed.");
    }

    if (g_settingsUi) g_settingsUi->CloseAll();
    if (g_mqttThread) {
        g_mqttThread->request_stop();
        g_mqttThread->join();
        g_mqttThread.reset();
    }

	// Stop WinRT-based config watcher
	StopConfigWatcher();

    g_tray.Destroy();
    g_settingsUi.reset();
    if (hMutex) CloseHandle(hMutex);
    spdlog::info("=== Application Terminating ===");
    spdlog::shutdown();
    uninit_apartment();
    return g_exitCode;
}

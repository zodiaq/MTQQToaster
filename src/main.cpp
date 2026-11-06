#include "app_logic.h"
#include "modern_ui.h"
#include <thread>
#include <atomic>
#include <filesystem>
#include <string_view>
#include <chrono>
#include <stdexcept>
#include <mosquitto.h>
#include <fmt/ranges.h> // fmt::join に必要

#include <winrt/Windows.Foundation.h>
#include <winrt/windows.foundation.collections.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Search.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Windows.UI.Xaml.Interop.h>

#include <MddBootstrap.h>
#include <WindowsAppSDK-VersionInfo.h>

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

bool ValidateMqttUrl(const std::string& value)
{
    try {
        const Windows::Foundation::Uri uri(to_hstring(value));
        const auto scheme = uri.SchemeName();
        return (scheme == L"mqtt" || scheme == L"mqtts") && !uri.Host().empty() && uri.Port() > 0;
    }
    catch (const hresult_error&) {
        return false;
    }
}

AppConfig GetConfigSnapshot()
{
    AppConfig config;
    AcquireSRWLockShared(&g_configLock);
    config = g_config;
    ReleaseSRWLockShared(&g_configLock);
    return config;
}

bool SaveModernSettings(const AppConfig& requestedConfig,
                        const std::vector<std::filesystem::path>& selectedImages,
                        std::wstring& error)
{
    AppConfig config = requestedConfig;
    if (!ValidateMqttUrl(config.url)) {
        error = L"MQTT URLには mqtt:// または mqtts:// の有効なURLを入力してください。";
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
    AcquireSRWLockExclusive(&g_configLock);
    g_config = config;
    ReleaseSRWLockExclusive(&g_configLock);
    UpdateLogLevel(config.logLevel);
    g_needsReconnect.store(true, std::memory_order_release);
    spdlog::info("Settings saved from the WinUI window.");
    return true;
}

}

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

    NotificationInfo data = ParseNotificationPayload(payload);
    if (data.imageUrl.empty() && msg->topic != nullptr) {
        AppConfig config;
        AcquireSRWLockShared(&g_configLock);
        config = g_config;
        ReleaseSRWLockShared(&g_configLock);

        for (const auto& topic : config.topics) {
            bool matches = false;
            if (mosquitto_topic_matches_sub(topic.topic.c_str(), msg->topic, &matches) == MOSQ_ERR_SUCCESS && matches) {
                if (!topic.image.empty()) data.imageUrl = topic.image;
                break;
            }
        }
    }
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
            std::vector<std::string> topicNames;
            topicNames.reserve(cfg.topics.size());
            for (const auto& topic : cfg.topics) topicNames.push_back(topic.topic);
            spdlog::info("Connected to MQTT Broker. Subscribing to topics: {}", fmt::join(topicNames, ", "));
            for (const auto& topic : cfg.topics) {
                mosquitto_subscribe(mosq, NULL, topic.topic.c_str(), 0);
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

namespace {

std::unique_ptr<ModernUi> g_modernUi;
TrayIcon g_tray;
std::unique_ptr<std::jthread> g_mqttThread;
HINSTANCE g_instance = nullptr;
bool g_uiSmokeTest = false;
int g_exitCode = 0;

struct App : winrt::Microsoft::UI::Xaml::ApplicationT<App, winrt::Microsoft::UI::Xaml::Markup::IXamlMetadataProvider> {
    App()
    {
        UnhandledException([](auto&&, const winrt::Microsoft::UI::Xaml::UnhandledExceptionEventArgs& args) {
            spdlog::critical("Unhandled XAML exception: {}", to_string(args.Message()));
        });
    }

    winrt::Microsoft::UI::Xaml::Markup::IXamlType GetXamlType(
        const Windows::UI::Xaml::Interop::TypeName& type) { return MetadataProvider().GetXamlType(type); }
    winrt::Microsoft::UI::Xaml::Markup::IXamlType GetXamlType(const hstring& name) {
        return MetadataProvider().GetXamlType(name);
    }
    com_array<winrt::Microsoft::UI::Xaml::Markup::XmlnsDefinition> GetXmlnsDefinitions() {
        return MetadataProvider().GetXmlnsDefinitions();
    }

    void OnLaunched(const winrt::Microsoft::UI::Xaml::LaunchActivatedEventArgs&)
    {
        try {
            Launch();
        }
        catch (const hresult_error& ex) {
            g_exitCode = 1;
            spdlog::critical("Application launch failed. HRESULT: 0x{:08x}, message: {}",
                             static_cast<unsigned int>(ex.code()), to_string(ex.message()));
            Exit();
        }
        catch (const std::exception& ex) {
            g_exitCode = 1;
            spdlog::critical("Application launch failed: {}", ex.what());
            Exit();
        }
    }

private:
    void Launch()
    {
        // XAML schema initialization requires a fully constructed Application.
        spdlog::info("Initializing WinUI metadata.");
        winrt::Microsoft::UI::Xaml::XamlTypeInfo::XamlControlsXamlMetaDataProvider::Initialize();
        spdlog::info("Initializing WinUI control resources.");
        Resources().MergedDictionaries().Append(
            winrt::Microsoft::UI::Xaml::Controls::XamlControlsResources());
        spdlog::info("WinUI resources initialized.");

        // This tray app must keep running when the settings window closes.
        DispatcherShutdownMode(winrt::Microsoft::UI::Xaml::DispatcherShutdownMode::OnExplicitShutdown);

        ModernUiCallbacks callbacks;
        if (g_uiSmokeTest) {
            callbacks.loadConfig = [] {
                AppConfig config;
                config.url = "mqtt://localhost:1883";
                config.topics.push_back({"test/日本語", ""});
                return config;
            };
            callbacks.saveConfig = [](auto&&, auto&&, auto&&) { return false; };
            callbacks.loadPlacement = [] { return LoadSettingsWindowPlacement(GetModuleDirectory() / L"ui-smoke-state.json"); };
            callbacks.savePlacement = [](const auto& placement) {
                if (!SaveSettingsWindowPlacement(GetModuleDirectory() / L"ui-smoke-state.json", placement)) {
                    throw std::runtime_error("Could not save UI smoke placement");
                }
            };
        }
        else {
            callbacks.loadConfig = GetConfigSnapshot;
            callbacks.saveConfig = SaveModernSettings;
            const auto placementPath = g_appDataPaths.configDirectory / L"ui-state.json";
            callbacks.loadPlacement = [placementPath] { return LoadSettingsWindowPlacement(placementPath); };
            callbacks.savePlacement = [placementPath](const auto& placement) {
                if (!SaveSettingsWindowPlacement(placementPath, placement)) {
                    spdlog::warn("Could not save settings window placement.");
                }
            };
        }
        g_modernUi = std::make_unique<ModernUi>(std::move(callbacks));

        if (g_uiSmokeTest) {
            StartUiSmokeTest();
            return;
        }

        if (!g_tray.Create(
                g_instance,
                [] { if (g_modernUi) g_modernUi->ShowSettings((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0); },
                [] {
                    spdlog::info("Exit selected from tray menu. Terminating...");
                    winrt::Microsoft::UI::Xaml::Application::Current().Exit();
                })) {
            spdlog::error("Failed to create tray icon.");
        }

        StartConfigWatcherAsync(g_appDataPaths.configDirectory.wstring(), L"config.json");
        g_mqttThread = std::make_unique<std::jthread>(MqttWorkerThread);
        spdlog::info("Tray application ready.");
    }

    void StartUiSmokeTest()
    {
        smokeTimer = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread().CreateTimer();
        smokeTimer.Interval(std::chrono::seconds(1));
        smokeTimer.IsRepeating(true);
        smokeTimer.Tick([this](auto&&, auto&&) {
            try {
                switch (smokeStep++) {
                case 0:
                    spdlog::info("UI smoke test: opening settings after windowless startup.");
                    g_modernUi->ShowSettings(true);
                    break;
                case 1:
                    if (!g_modernUi->IsDefaultLayoutReady()) throw std::runtime_error("Default settings layout is incorrect");
                    {
                        const auto hwnd = g_modernUi->SettingsHandle();
                        check_bool(GetWindowRect(hwnd, &smokeBounds));
                        check_bool(SetWindowPos(hwnd, nullptr, smokeBounds.left + 12, smokeBounds.top + 12,
                            smokeBounds.right - smokeBounds.left - 40, smokeBounds.bottom - smokeBounds.top - 40,
                            SWP_NOZORDER | SWP_NOACTIVATE));
                        check_bool(GetWindowRect(hwnd, &smokeBounds));
                    }
                    break;
                case 2:
                    g_modernUi->CloseAll();
                    break;
                case 3:
                    g_modernUi->ShowSettings();
                    break;
                case 4:
                    if (!g_modernUi->IsSettingsReady()) throw std::runtime_error("Settings controls did not reload");
                    {
                        RECT restored{};
                        check_bool(GetWindowRect(g_modernUi->SettingsHandle(), &restored));
                        if (!EqualRect(&smokeBounds, &restored)) throw std::runtime_error("Window placement was not restored");
                    }
                    spdlog::info("UI smoke test: saved position and size restored.");
                    g_modernUi->ShowSettings(true);
                    break;
                case 5:
                    if (!g_modernUi->IsDefaultLayoutReady()) throw std::runtime_error("Window placement reset failed");
                    spdlog::info("UI smoke test: default placement reset passed.");
                    g_modernUi->CloseAll();
                    break;
                case 6:
                    smokeTimer.Stop();
                    g_exitCode = 0;
                    spdlog::info("UI smoke test passed.");
                    Exit();
                    break;
                }
            }
            catch (const hresult_error& ex) {
                spdlog::error("UI smoke test failed. HRESULT: 0x{:08x}, message: {}",
                              static_cast<unsigned int>(ex.code()), to_string(ex.message()));
                smokeTimer.Stop();
                Exit();
            }
            catch (const std::exception& ex) {
                spdlog::error("UI smoke test failed: {}", ex.what());
                smokeTimer.Stop();
                Exit();
            }
        });
        smokeTimer.Start();
        spdlog::info("UI smoke test: waiting for UI dispatcher.");
    }

    winrt::Microsoft::UI::Xaml::XamlTypeInfo::XamlControlsXamlMetaDataProvider MetadataProvider()
    {
        if (!metadataProvider) {
            metadataProvider = winrt::Microsoft::UI::Xaml::XamlTypeInfo::XamlControlsXamlMetaDataProvider();
        }
        return metadataProvider;
    }

    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer smokeTimer{nullptr};
    int smokeStep = 0;
    RECT smokeBounds{};
    winrt::Microsoft::UI::Xaml::XamlTypeInfo::XamlControlsXamlMetaDataProvider metadataProvider{nullptr};
};

}

int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPSTR commandLine, _In_ int) {
    g_uiSmokeTest = std::string_view(commandLine) == "--ui-smoke-test";
    g_exitCode = g_uiSmokeTest ? 1 : 0;
    init_apartment(apartment_type::single_threaded);

    const HRESULT bootstrapResult = MddBootstrapInitialize2(
        WINDOWSAPPSDK_RELEASE_MAJORMINOR,
        WINDOWSAPPSDK_RELEASE_VERSION_TAG_W,
        PACKAGE_VERSION{WINDOWSAPPSDK_RUNTIME_VERSION_UINT64},
        MddBootstrapInitializeOptions_OnPackageIdentity_NOOP);
    if (FAILED(bootstrapResult)) {
        MessageBoxW(nullptr, L"Windows App SDK Runtimeを初期化できませんでした。",
                    L"MTQQToaster", MB_ICONERROR | MB_OK);
        uninit_apartment();
        return static_cast<int>(bootstrapResult);
    }

    g_instance = hInstance;

	// preventing multiple instances
	HANDLE hMutex = g_uiSmokeTest ? nullptr : CreateMutexW(NULL, TRUE, L"Global\\MQTTToasterMutex");
	if (!g_uiSmokeTest && GetLastError() == ERROR_ALREADY_EXISTS) {
		MessageBoxW(NULL, L"MQTT Toaster is already running.", L"MQTT Toaster", MB_ICONEXCLAMATION | MB_OK);
		MddBootstrapShutdown();
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
            MddBootstrapShutdown();
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

    winrt::Microsoft::UI::Xaml::Application::Start([](auto&&) { make<App>(); });

    if (g_modernUi) g_modernUi->CloseAll();
    if (g_mqttThread) {
        g_mqttThread->request_stop();
        g_mqttThread->join();
        g_mqttThread.reset();
    }

	// Stop WinRT-based config watcher
	StopConfigWatcher();

    g_tray.Destroy();
    g_modernUi.reset();
    if (hMutex) CloseHandle(hMutex);
    spdlog::info("=== Application Terminating ===");
    spdlog::shutdown();

    MddBootstrapShutdown();
    uninit_apartment();
    return g_exitCode;
}

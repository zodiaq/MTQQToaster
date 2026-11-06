#include "app_logic.h"
#include "resource.h"
#include <thread>
#include <atomic>
#include <filesystem>
#include <sstream>
#include <commdlg.h>
#include <wincodec.h>
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
AppStoragePaths g_appDataPaths;

namespace {

struct TopicDialogState {
    TopicConfig topic;
    std::filesystem::path selectedImageSource;
    HBITMAP previewBitmap = nullptr;
};

struct SettingsDialogState {
    AppConfig config;
    std::vector<std::filesystem::path> selectedImageSources;
};

std::wstring GetDialogText(HWND dialog, int controlId)
{
    const int length = GetWindowTextLengthW(GetDlgItem(dialog, controlId));
    std::wstring value(static_cast<size_t>(length) + 1, L'\0');
    GetDlgItemTextW(dialog, controlId, value.data(), length + 1);
    value.resize(static_cast<size_t>(length));
    return value;
}

void SetDialogText(HWND dialog, int controlId, const std::string& value)
{
    SetDlgItemTextW(dialog, controlId, winrt::to_hstring(value).c_str());
}

void RefreshTopicList(HWND dialog, const SettingsDialogState& state)
{
    const HWND list = GetDlgItem(dialog, IDC_TOPIC_LIST);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    for (const auto& topic : state.config.topics) {
        std::wstring label = winrt::to_hstring(topic.topic).c_str();
        if (!topic.image.empty()) label += L"  [画像あり]";
        SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
}

HBITMAP CreateImageThumbnail(const std::filesystem::path& imagePath, UINT maxWidth, UINT maxHeight)
{
    winrt::com_ptr<IWICImagingFactory> factory;
    winrt::check_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                         IID_PPV_ARGS(factory.put())));

    winrt::com_ptr<IWICBitmapDecoder> decoder;
    winrt::check_hresult(factory->CreateDecoderFromFilename(imagePath.c_str(), nullptr, GENERIC_READ,
                                                            WICDecodeMetadataCacheOnLoad, decoder.put()));
    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    winrt::check_hresult(decoder->GetFrame(0, frame.put()));

    UINT sourceWidth = 0;
    UINT sourceHeight = 0;
    winrt::check_hresult(frame->GetSize(&sourceWidth, &sourceHeight));
    if (sourceWidth == 0 || sourceHeight == 0) return nullptr;

    const double scale = (std::min)(static_cast<double>(maxWidth) / sourceWidth,
                                    static_cast<double>(maxHeight) / sourceHeight);
    const UINT width = (std::max)(1U, static_cast<UINT>(sourceWidth * (std::min)(scale, 1.0)));
    const UINT height = (std::max)(1U, static_cast<UINT>(sourceHeight * (std::min)(scale, 1.0)));

    winrt::com_ptr<IWICBitmapScaler> scaler;
    winrt::check_hresult(factory->CreateBitmapScaler(scaler.put()));
    winrt::check_hresult(scaler->Initialize(frame.get(), width, height, WICBitmapInterpolationModeFant));

    winrt::com_ptr<IWICFormatConverter> converter;
    winrt::check_hresult(factory->CreateFormatConverter(converter.put()));
    winrt::check_hresult(converter->Initialize(scaler.get(), GUID_WICPixelFormat32bppPBGRA,
                                               WICBitmapDitherTypeNone, nullptr, 0.0,
                                               WICBitmapPaletteTypeCustom));

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = static_cast<LONG>(width);
    bitmapInfo.bmiHeader.biHeight = -static_cast<LONG>(height);
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &bitmapInfo, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap) return nullptr;

    const UINT stride = width * 4;
    const HRESULT result = converter->CopyPixels(nullptr, stride, stride * height,
                                                 static_cast<BYTE*>(pixels));
    if (FAILED(result)) {
        DeleteObject(bitmap);
        winrt::check_hresult(result);
    }
    return bitmap;
}

void UpdateImageThumbnail(HWND dialog, TopicDialogState& state)
{
    std::filesystem::path imagePath = state.selectedImageSource;
    if (imagePath.empty() && !state.topic.image.empty()) {
        imagePath = g_appDataPaths.configDirectory / winrt::to_hstring(state.topic.image).c_str();
    }

    HBITMAP newBitmap = nullptr;
    if (!imagePath.empty()) {
        try {
            newBitmap = CreateImageThumbnail(imagePath, 116, 71);
        }
        catch (const winrt::hresult_error&) {
            newBitmap = nullptr;
        }
    }

    SendDlgItemMessageW(dialog, IDC_TOPIC_IMAGE, STM_SETIMAGE, IMAGE_BITMAP,
                        reinterpret_cast<LPARAM>(newBitmap));
    if (state.previewBitmap) DeleteObject(state.previewBitmap);
    state.previewBitmap = newBitmap;
    SetDlgItemTextW(dialog, IDC_IMAGE_STATUS,
                    imagePath.empty() ? L"画像なし" : (newBitmap ? L"プレビュー" : L"表示できません"));
}

INT_PTR CALLBACK TopicDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* state = reinterpret_cast<TopicDialogState*>(GetWindowLongPtrW(dialog, DWLP_USER));
    try {
        if (message == WM_INITDIALOG) {
            state = reinterpret_cast<TopicDialogState*>(lParam);
            SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
            SetDialogText(dialog, IDC_TOPIC_NAME, state->topic.topic);
            UpdateImageThumbnail(dialog, *state);
            return TRUE;
        }
        if (message == WM_DESTROY && state != nullptr) {
            SendDlgItemMessageW(dialog, IDC_TOPIC_IMAGE, STM_SETIMAGE, IMAGE_BITMAP, 0);
            if (state->previewBitmap) {
                DeleteObject(state->previewBitmap);
                state->previewBitmap = nullptr;
            }
            return TRUE;
        }
        if (message != WM_COMMAND || state == nullptr) return FALSE;

        if (LOWORD(wParam) == IDC_IMAGE_BROWSE) {
            std::wstring fileName(32768, L'\0');
            OPENFILENAMEW options{};
            options.lStructSize = sizeof(options);
            options.hwndOwner = dialog;
            options.lpstrFilter = L"画像ファイル (*.png;*.jpg;*.jpeg;*.gif)\0*.png;*.jpg;*.jpeg;*.gif\0すべてのファイル\0*.*\0";
            options.lpstrFile = fileName.data();
            options.nMaxFile = static_cast<DWORD>(fileName.size());
            options.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameW(&options)) {
                fileName.resize(wcslen(fileName.c_str()));
                state->selectedImageSource = fileName;
                UpdateImageThumbnail(dialog, *state);
            }
            return TRUE;
        }
        if (LOWORD(wParam) == IDC_IMAGE_CLEAR) {
            state->topic.image.clear();
            state->selectedImageSource.clear();
            UpdateImageThumbnail(dialog, *state);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        if (LOWORD(wParam) == IDOK) {
            const auto topic = GetDialogText(dialog, IDC_TOPIC_NAME);
            const auto first = topic.find_first_not_of(L" \t");
            const auto last = topic.find_last_not_of(L" \t");
            if (first == std::wstring::npos) {
                MessageBoxW(dialog, L"topicを入力してください。", L"入力エラー", MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            state->topic.topic = winrt::to_string(winrt::hstring(topic.substr(first, last - first + 1)));
            EndDialog(dialog, IDOK);
            return TRUE;
        }
    }
    catch (...) {
        MessageBoxW(dialog, L"topic設定の処理中にエラーが発生しました。", L"MTQQToaster", MB_OK | MB_ICONERROR);
        return TRUE;
    }
    return FALSE;
}

bool ReadPositiveNumber(HWND dialog, int controlId, size_t& value)
{
    const auto text = GetDialogText(dialog, controlId);
    wchar_t* end = nullptr;
    const unsigned long parsed = std::wcstoul(text.c_str(), &end, 10);
    if (text.empty() || end == text.c_str() || *end != L'\0' || parsed == 0) return false;
    value = static_cast<size_t>(parsed);
    return true;
}

bool ValidateMqttUrl(const std::string& value)
{
    try {
        const Windows::Foundation::Uri uri(winrt::to_hstring(value));
        const auto scheme = uri.SchemeName();
        return (scheme == L"mqtt" || scheme == L"mqtts") && !uri.Host().empty() && uri.Port() > 0;
    }
    catch (const winrt::hresult_error&) {
        return false;
    }
}

INT_PTR CALLBACK SettingsDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* state = reinterpret_cast<SettingsDialogState*>(GetWindowLongPtrW(dialog, DWLP_USER));
    try {
        if (message == WM_INITDIALOG) {
            state = reinterpret_cast<SettingsDialogState*>(lParam);
            SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
            const auto& config = state->config;

            SetDialogText(dialog, IDC_MQTT_URL, config.url);
            RefreshTopicList(dialog, *state);
            SetDialogText(dialog, IDC_MQTT_USER, config.user);
            SetDialogText(dialog, IDC_MQTT_PASS, config.pass);
            SetDlgItemInt(dialog, IDC_LOG_SIZE, static_cast<UINT>(config.maxLogSizeMb), FALSE);
            SetDlgItemInt(dialog, IDC_LOG_FILES, static_cast<UINT>(config.maxLogFiles), FALSE);

            const HWND logLevel = GetDlgItem(dialog, IDC_LOG_LEVEL);
            constexpr const wchar_t* levels[] = { L"TRACE", L"DEBUG", L"INFO", L"WARN", L"ERROR", L"NONE" };
            int selected = 2;
            for (int i = 0; i < static_cast<int>(std::size(levels)); ++i) {
                SendMessageW(logLevel, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(levels[i]));
                if (config.logLevel == winrt::to_string(winrt::hstring(levels[i]))) selected = i;
            }
            SendMessageW(logLevel, CB_SETCURSEL, selected, 0);
            return TRUE;
        }

        if (message != WM_COMMAND || state == nullptr) return FALSE;
        if (LOWORD(wParam) == IDC_TOPIC_ADD) {
            TopicDialogState topicState;
            if (DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_TOPIC), dialog,
                                TopicDialogProc, reinterpret_cast<LPARAM>(&topicState)) == IDOK) {
                state->config.topics.push_back(std::move(topicState.topic));
                state->selectedImageSources.push_back(std::move(topicState.selectedImageSource));
                RefreshTopicList(dialog, *state);
            }
            return TRUE;
        }
        if (LOWORD(wParam) == IDC_TOPIC_EDIT) {
            const auto selected = SendDlgItemMessageW(dialog, IDC_TOPIC_LIST, LB_GETCURSEL, 0, 0);
            if (selected == LB_ERR) return TRUE;
            TopicDialogState topicState{state->config.topics[static_cast<size_t>(selected)],
                                        state->selectedImageSources[static_cast<size_t>(selected)]};
            if (DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_TOPIC), dialog,
                                TopicDialogProc, reinterpret_cast<LPARAM>(&topicState)) == IDOK) {
                state->config.topics[static_cast<size_t>(selected)] = std::move(topicState.topic);
                state->selectedImageSources[static_cast<size_t>(selected)] = std::move(topicState.selectedImageSource);
                RefreshTopicList(dialog, *state);
                SendDlgItemMessageW(dialog, IDC_TOPIC_LIST, LB_SETCURSEL, selected, 0);
            }
            return TRUE;
        }
        if (LOWORD(wParam) == IDC_TOPIC_DELETE) {
            const auto selected = SendDlgItemMessageW(dialog, IDC_TOPIC_LIST, LB_GETCURSEL, 0, 0);
            if (selected == LB_ERR) return TRUE;
            state->config.topics.erase(state->config.topics.begin() + selected);
            state->selectedImageSources.erase(state->selectedImageSources.begin() + selected);
            RefreshTopicList(dialog, *state);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        if (LOWORD(wParam) != IDOK) return FALSE;

        AppConfig config = state->config;
        config.url = winrt::to_string(winrt::hstring(GetDialogText(dialog, IDC_MQTT_URL)));
        config.user = winrt::to_string(winrt::hstring(GetDialogText(dialog, IDC_MQTT_USER)));
        config.pass = winrt::to_string(winrt::hstring(GetDialogText(dialog, IDC_MQTT_PASS)));

        wchar_t logLevel[16]{};
        GetDlgItemTextW(dialog, IDC_LOG_LEVEL, logLevel, static_cast<int>(std::size(logLevel)));
        config.logLevel = winrt::to_string(winrt::hstring(logLevel));

        if (!ValidateMqttUrl(config.url)) {
            MessageBoxW(dialog, L"MQTT URLには mqtt:// または mqtts:// の有効なURLを入力してください。", L"入力エラー", MB_OK | MB_ICONWARNING);
            return TRUE;
        }
        if (config.topics.empty()) {
            MessageBoxW(dialog, L"トピックを1件以上入力してください。", L"入力エラー", MB_OK | MB_ICONWARNING);
            return TRUE;
        }
        if (!ReadPositiveNumber(dialog, IDC_LOG_SIZE, config.maxLogSizeMb) ||
            !ReadPositiveNumber(dialog, IDC_LOG_FILES, config.maxLogFiles)) {
            MessageBoxW(dialog, L"ログサイズと保持数には1以上の整数を入力してください。", L"入力エラー", MB_OK | MB_ICONWARNING);
            return TRUE;
        }

        AppConfig activeConfig;
        AcquireSRWLockShared(&g_configLock);
        activeConfig = g_config;
        ReleaseSRWLockShared(&g_configLock);

        for (size_t i = 0; i < state->selectedImageSources.size(); ++i) {
            if (state->selectedImageSources[i].empty()) continue;
            std::string copiedFile;
            if (!CopyToastImage(g_appDataPaths.configDirectory, state->selectedImageSources[i], copiedFile)) {
                RemoveUnusedToastImages(g_appDataPaths.configDirectory, activeConfig.topics);
                MessageBoxW(dialog, L"画像を設定フォルダーへコピーできませんでした。PNG、JPEG、GIFを選択してください。",
                            L"画像エラー", MB_OK | MB_ICONERROR);
                return TRUE;
            }
            config.topics[i].image = copiedFile;
        }

        if (!SaveConfig(g_appDataPaths.configPath, config)) {
            RemoveUnusedToastImages(g_appDataPaths.configDirectory, activeConfig.topics);
            MessageBoxW(dialog, L"設定を保存できませんでした。", L"保存エラー", MB_OK | MB_ICONERROR);
            return TRUE;
        }

        RemoveUnusedToastImages(g_appDataPaths.configDirectory, config.topics);

        AppConfig publishedConfig = config;
        AcquireSRWLockExclusive(&g_configLock);
        g_config = std::move(publishedConfig);
        ReleaseSRWLockExclusive(&g_configLock);
        UpdateLogLevel(config.logLevel);
        g_needsReconnect.store(true, std::memory_order_release);
        spdlog::info("Settings saved from the dialog.");
        EndDialog(dialog, IDOK);
        return TRUE;
    }
    catch (const std::exception& ex) {
        spdlog::error("Settings dialog error: {}", ex.what());
        MessageBoxW(dialog, L"設定処理中にエラーが発生しました。", L"MTQQToaster", MB_OK | MB_ICONERROR);
        return TRUE;
    }
    catch (const winrt::hresult_error& ex) {
        spdlog::error("Settings dialog HRESULT: {:#x}", static_cast<unsigned int>(ex.code().value));
        MessageBoxW(dialog, L"設定処理中にWindows APIエラーが発生しました。", L"MTQQToaster", MB_OK | MB_ICONERROR);
        return TRUE;
    }
}

void ShowSettingsDialog(HWND owner)
{
    SettingsDialogState state;
    AcquireSRWLockShared(&g_configLock);
    state.config = g_config;
    ReleaseSRWLockShared(&g_configLock);
    state.selectedImageSources.resize(state.config.topics.size());
    DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SETTINGS), owner,
                    SettingsDialogProc, reinterpret_cast<LPARAM>(&state));
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

int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR lpCmdLine, _In_ int nCmdShow) {
    init_apartment(apartment_type::single_threaded);

	// preventing multiple instances
	HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Global\\MQTTToasterMutex");
	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		MessageBoxW(NULL, L"MQTT Toaster is already running.", L"MQTT Toaster", MB_ICONEXCLAMATION | MB_OK);
		return 0;
	}

    g_appDataPaths = GetAppDataPaths();
    if (!InitializeAppData(g_appDataPaths)) {
        MessageBoxW(NULL, L"アプリケーションデータフォルダーを初期化できませんでした。", L"MTQQToaster", MB_ICONERROR | MB_OK);
        return 1;
    }

    const std::wstring configFileName = L"config.json";
    const auto& configPath = g_appDataPaths.configPath;

    size_t initLogSizeMb = 5, initLogMaxFiles = 3;
    ReadLogConfigFirst(configPath, initLogSizeMb, initLogMaxFiles);
    InitLogger(g_appDataPaths.logPath, initLogSizeMb, initLogMaxFiles);

    spdlog::info("=== Application Started ===");
    if (!LoadConfig(configPath, g_config)) {
        SaveConfig(configPath, g_config);
    }

    TrayIcon tray;
    if (!tray.Create(hInstance, ShowSettingsDialog)) spdlog::error("Failed to create tray icon.");

	// start WinRT-based config watcher
    StartConfigWatcherAsync(g_appDataPaths.configDirectory.wstring(), configFileName);

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

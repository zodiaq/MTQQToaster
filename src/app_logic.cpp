#include "app_logic.h"
#include "resource.h"
#include <fstream>
#include <cstdlib>
#include <unordered_set>
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")

#include <nlohmann/json.hpp>
#include <spdlog/sinks/rotating_file_sink.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.UI.Notifications.h>
#include <winrt/Windows.Storage.h>

#define MQTTTOASTER_REGVAL_NAME L"MQTTToaster"
#define STARTUP_REGISTRY_PATH L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"

using json = nlohmann::json;
using namespace winrt;
using namespace Windows::Data::Xml::Dom;
using namespace Windows::UI::Notifications;

#define WM_TRAYICON (WM_USER + 1)

static std::filesystem::path GetModulePath()
{
	std::wstring path(MAX_PATH, L'\0');
	while (true)
	{
        DWORD size = path.size();
		BOOL result = QueryFullProcessImageNameW(GetCurrentProcess(), 0, path.data(), &size);
        if (size == 0) return L""; // エラー処理
		if (size < path.size())
		{
			path.resize(size);
			break;
		}
		path.resize(path.size() * 2); // バッファが足りない場合は倍に拡張
	}
	return std::filesystem::path(path);
}


std::filesystem::path GetModuleDirectory()
{
    return GetModulePath().parent_path();
}

AppStoragePaths GetAppDataPaths()
{
    AppStoragePaths paths;
    try {
        const auto appData = winrt::Windows::Storage::ApplicationData::Current();
        paths.configDirectory = appData.LocalFolder().Path().c_str();
        paths.cacheDirectory = appData.LocalCacheFolder().Path().c_str();
    }
    catch (const winrt::hresult_error&) {
        wchar_t* localAppData = nullptr;
        size_t length = 0;
        if (_wdupenv_s(&localAppData, &length, L"LOCALAPPDATA") == 0 && localAppData != nullptr) {
            paths.configDirectory = std::filesystem::path(localAppData) / L"MTQQToaster";
            free(localAppData);
        }
        else {
            paths.configDirectory = GetModuleDirectory();
        }
        paths.cacheDirectory = paths.configDirectory;
    }

    paths.toastAssetsDirectory = paths.configDirectory / L"ToastAssets";
    paths.configPath = paths.configDirectory / L"config.json";
    paths.logPath = paths.cacheDirectory / L"Logs" / L"app.log";
    return paths;
}

bool InitializeAppData(const AppStoragePaths& paths)
{
    std::error_code ec;
    std::filesystem::create_directories(paths.configDirectory, ec);
    if (ec) return false;
    std::filesystem::create_directories(paths.logPath.parent_path(), ec);
    if (ec) return false;
    std::filesystem::create_directories(paths.toastAssetsDirectory, ec);
    if (ec) return false;

    if (std::filesystem::exists(paths.configPath, ec)) return true;

    const auto moduleDirectory = GetModuleDirectory();
    const auto legacyConfig = moduleDirectory / L"config.json";
    const auto sampleConfig = moduleDirectory / L"config.sample.json";
    const auto source = std::filesystem::exists(legacyConfig, ec) ? legacyConfig : sampleConfig;
    if (!std::filesystem::exists(source, ec)) return true;

    std::filesystem::copy_file(source, paths.configPath, std::filesystem::copy_options::none, ec);
    return !ec;
}

// MQTTからのペイロード（JSON or Text）をパースする関数
NotificationInfo ParseNotificationPayload(const std::string& payload) {
    NotificationInfo data;
    data.message = payload;

    try {
        auto j = json::parse(payload);
        if (j.contains("title") && j["title"].is_string()) data.title = j["title"].get<std::string>();
        if (j.contains("message") && j["message"].is_string()) data.message = j["message"].get<std::string>();
        if (j.contains("image") && j["image"].is_string()) data.imageUrl = j["image"].get<std::string>();
        else if (j.contains("image_url") && j["image_url"].is_string()) data.imageUrl = j["image_url"].get<std::string>();
    }
    catch (const json::parse_error&) {
        // パースエラー時はプレーンテキストとして処理
    }

    return data;
}

void ReadLogConfigFirst(const std::filesystem::path& configPath, size_t& outSizeMb, size_t& outMaxFiles) {
    std::ifstream f(configPath);
    if (!f.is_open()) return;

    try {
        json j;
        f >> j;
        if (j.contains("log_max_size_mb") && j["log_max_size_mb"].is_number_integer()) {
            outSizeMb = j["log_max_size_mb"].get<size_t>();
        }
        if (j.contains("log_max_files") && j["log_max_files"].is_number_integer()) {
            outMaxFiles = j["log_max_files"].get<size_t>();
        }
    }
    catch (...) {}
}

void InitLogger(const std::filesystem::path& logPath, size_t maxMb, size_t maxFiles) {
    try {
        spdlog::filename_t logPathStr(logPath.native());
        size_t maxSizeBytes = maxMb * 1024 * 1024;

        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(logPathStr, maxSizeBytes, maxFiles);
        auto logger = std::make_shared<spdlog::logger>("global", file_sink);
        spdlog::set_default_logger(logger);

        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S] [%l] %v");
        spdlog::flush_on(spdlog::level::trace);
    }
    catch (const spdlog::spdlog_ex&) {}
}

void UpdateLogLevel(const std::string& levelStr) {
	const std::unordered_map<std::string, spdlog::level::level_enum> levelMap = {
		{"TRACE", spdlog::level::trace},
		{"DEBUG", spdlog::level::debug},
		{"INFO",  spdlog::level::info},
		{"WARN",  spdlog::level::warn},
		{"ERROR", spdlog::level::err},
		{"NONE",  spdlog::level::off}
	};
    auto it = levelMap.find(levelStr);
    if (it != levelMap.end()) {
        spdlog::set_level(it->second);
    }
    else spdlog::set_level(spdlog::level::info);
}

bool LoadConfig(const std::filesystem::path& configPath, AppConfig& outConfig) {
    std::ifstream f(configPath);
    if (!f.is_open()) return false;

    try {
        json j;
        f >> j;

        AppConfig newConfig;
        if (j.contains("mqtt_url") && j["mqtt_url"].is_string()) {
            newConfig.url = j["mqtt_url"].get<std::string>();
        }
        if (j.contains("mqtt_topics") && j["mqtt_topics"].is_array()) {
			std::vector<TopicConfig> topics;
			for (const auto& topic : j["mqtt_topics"])
            {
				if (topic.is_string()) {
					topics.push_back({topic.get<std::string>(), ""});
				}
                else if (topic.is_object() && topic.contains("topic") && topic["topic"].is_string()) {
                    TopicConfig topicConfig;
                    topicConfig.topic = topic["topic"].get<std::string>();
                    if (topic.contains("image") && topic["image"].is_string()) {
                        const auto image = std::filesystem::path(winrt::to_hstring(topic["image"].get<std::string>()).c_str());
                        const auto fileName = image.filename().wstring();
                        if (image == image.filename() && fileName.starts_with(L"topic-image-")) {
                            topicConfig.image = winrt::to_string(winrt::hstring(fileName));
                        }
                    }
                    if (!topicConfig.topic.empty()) topics.push_back(std::move(topicConfig));
				}
            }
			if (!topics.empty()) {
				newConfig.topics = topics;
			}
        }
        if (j.contains("mqtt_user") && j["mqtt_user"].is_string()) newConfig.user = j["mqtt_user"].get<std::string>();
        if (j.contains("mqtt_pass") && j["mqtt_pass"].is_string()) newConfig.pass = j["mqtt_pass"].get<std::string>();
        if (j.contains("log_level") && j["log_level"].is_string()) newConfig.logLevel = j["log_level"].get<std::string>();
        if (j.contains("log_max_size_mb") && j["log_max_size_mb"].is_number_integer()) newConfig.maxLogSizeMb = j["log_max_size_mb"].get<size_t>();
        if (j.contains("log_max_files") && j["log_max_files"].is_number_integer()) newConfig.maxLogFiles = j["log_max_files"].get<size_t>();

        UpdateLogLevel(newConfig.logLevel);
        outConfig = newConfig;

        return true;
    }
    catch (const std::exception& ex) {
        spdlog::error("JSON parse error: {}", ex.what());
        return false;
    }
}

bool SaveConfig(const std::filesystem::path& configPath, const AppConfig& config) {
    if (config.url.empty() || config.topics.empty() || config.maxLogSizeMb == 0 || config.maxLogFiles == 0) {
        return false;
    }

    json topicList = json::array();
    for (const auto& topic : config.topics) {
        topicList.push_back({{"topic", topic.topic}, {"image", topic.image}});
    }

    json j = {
        {"mqtt_url", config.url},
        {"mqtt_topics", std::move(topicList)},
        {"mqtt_user", config.user},
        {"mqtt_pass", config.pass},
        {"log_level", config.logLevel},
        {"log_max_size_mb", config.maxLogSizeMb},
        {"log_max_files", config.maxLogFiles}
    };

    const auto temporaryPath = configPath.parent_path() / (configPath.filename().wstring() + L".tmp");
    try {
        std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
        if (!output.is_open()) return false;
        output << j.dump(2) << '\n';
        output.close();
        if (!output) return false;

        if (!MoveFileExW(temporaryPath.c_str(), configPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            std::error_code ec;
            std::filesystem::remove(temporaryPath, ec);
            return false;
        }
        return true;
    }
    catch (const std::exception&) {
        std::error_code ec;
        std::filesystem::remove(temporaryPath, ec);
        return false;
    }
}

bool CopyToastImage(const std::filesystem::path& configDirectory,
                    const std::filesystem::path& sourcePath,
                    std::string& outFileName) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(sourcePath, ec) || ec) return false;

    auto extension = sourcePath.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
    if (extension != L".png" && extension != L".jpg" && extension != L".jpeg" && extension != L".gif") {
        return false;
    }

    std::filesystem::create_directories(configDirectory, ec);
    if (ec) return false;

    const auto prefix = L"topic-image-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    std::filesystem::path destination;
    for (unsigned int suffix = 0; suffix < 100; ++suffix) {
        const auto name = prefix + (suffix == 0 ? L"" : L"-" + std::to_wstring(suffix)) + extension;
        destination = configDirectory / name;
        if (!std::filesystem::exists(destination, ec)) break;
    }

    std::filesystem::copy_file(sourcePath, destination, std::filesystem::copy_options::none, ec);
    if (ec) return false;
    outFileName = winrt::to_string(winrt::hstring(destination.filename().wstring()));
    return true;
}

void RemoveUnusedToastImages(const std::filesystem::path& configDirectory,
                             const std::vector<TopicConfig>& topics) {
    std::unordered_set<std::wstring> usedFiles;
    for (const auto& topic : topics) {
        if (!topic.image.empty()) {
            usedFiles.insert(winrt::to_hstring(topic.image).c_str());
        }
    }

    std::error_code ec;
    for (std::filesystem::directory_iterator it(configDirectory, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const auto fileName = it->path().filename().wstring();
        if (fileName.starts_with(L"topic-image-") && !usedFiles.contains(fileName)) {
            std::filesystem::remove(it->path(), ec);
            ec.clear();
        }
    }
}

std::wstring ResolveImagePath(const std::string& imageUrl) {
    if (imageUrl.empty()) {
        return L"";
    }

    auto wPath = winrt::to_hstring(imageUrl);

    // 1. 既に有効な URI かパースを試みる
    try {
        winrt::Windows::Foundation::Uri uri(wPath);
        auto scheme = uri.SchemeName();

        // http / https は不許可
        if (scheme == L"http" || scheme == L"https") {
            spdlog::error("HTTP/HTTPS image URLs are not supported: {}", imageUrl);
            return L"";
        }

        // file:// などの有効なスキームであれば、正規化された RawUri (または AbsoluteUri) を返す
        if (scheme == L"file") {
            return uri.RawUri().c_str();
        }
    }
    catch (const winrt::hresult_error&) {
        // パース失敗した場合はファイルパスとみなして後続の処理へ
    }

    if (wPath.starts_with(L"ms-appx:///") || wPath.starts_with(L"ms-appdata:///")) {
        return wPath.c_str();
    }

    // 2. std::filesystem でパスを解決（相対パスなら管理下のToastAssetsを優先）
    std::filesystem::path targetPath(wPath.c_str());
    if (targetPath.is_relative()) {
        const auto paths = GetAppDataPaths();
        const auto configuredImage = paths.configDirectory / targetPath;
        const auto appDataPath = paths.toastAssetsDirectory / targetPath;
        std::error_code ec;
        if (std::filesystem::exists(configuredImage, ec)) targetPath = configuredImage;
        else if (std::filesystem::exists(appDataPath, ec)) targetPath = appDataPath;
        else targetPath = GetModuleDirectory() / targetPath;
    }

    // 3. ローカル/UNCパスから安全に file:/// URI を作成
    // (空白や日本語のエスケープ、UNCのスラッシュ数も自動で完璧に処理される)
    try {
        winrt::Windows::Foundation::Uri fileUri(targetPath.c_str());
        return fileUri.RawUri().c_str();
    }
    catch (const winrt::hresult_error& ex) {
        spdlog::error("Failed to construct URI from path: {}, error: {:#x}", imageUrl, ex.code().value);
        return L"";
    }
}

void ShowToast(const NotificationInfo& data) {
    try {
        auto title = winrt::to_hstring(data.title);
        auto message = winrt::to_hstring(data.message);

        // 画像パスの解決 (exe横のファイルを優先参照)
        std::wstring imageUri = ResolveImagePath(data.imageUrl);

        XmlDocument toastXml = ToastNotificationManager::GetTemplateContent(
            imageUri.empty() ? ToastTemplateType::ToastText01 : ToastTemplateType::ToastImageAndText01
        );

        auto stringElements = toastXml.GetElementsByTagName(L"text");
        stringElements.Item(0).AppendChild(toastXml.CreateTextNode(message));

        if (!imageUri.empty()) {
            auto imageElements = toastXml.GetElementsByTagName(L"image");
            if (imageElements.Length() > 0) {
                auto imageNode = imageElements.Item(0);

                // src 属性の設定
                auto srcAttr = toastXml.CreateAttribute(L"src");
                srcAttr.Value(imageUri);
                imageNode.Attributes().SetNamedItem(srcAttr);

                // id 属性（1）の設定
                auto idAttr = toastXml.CreateAttribute(L"id");
                idAttr.Value(L"1");
                imageNode.Attributes().SetNamedItem(idAttr);
            }
        }

        ToastNotification toast(toastXml);
        auto notifier = ToastNotificationManager::CreateToastNotifier(winrt::to_hstring(data.title));
        notifier.Show(toast);

        spdlog::debug("Toast notification displayed: {}", data.title);
    }
    catch (const winrt::hresult_error& ex) {
        spdlog::error("Toast display error. HRESULT: 0x{:x}", static_cast<unsigned int>(ex.code()));
    }
}


static void ToggleStartupRegistration() {
	HKEY hKey;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, STARTUP_REGISTRY_PATH, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS) {
		bool isRegistered = false;
		if (RegQueryValueExW(hKey, MQTTTOASTER_REGVAL_NAME, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
			isRegistered = true;
		}
		if (isRegistered) {
			// 登録解除
			RegDeleteValueW(hKey, MQTTTOASTER_REGVAL_NAME);
			spdlog::info("Removed from startup.");
		}
		else {
			// 登録
            const std::wstring exePath = L"\"" + GetModulePath().wstring() + L"\"";
            RegSetValueExW(hKey, MQTTTOASTER_REGVAL_NAME, 0, REG_SZ, reinterpret_cast<const BYTE*>(exePath.c_str()), (exePath.size() + 1) * sizeof(wchar_t));
			spdlog::info("Added to startup: {}", std::string(exePath.begin(), exePath.end()));
		}
		RegCloseKey(hKey);
	}
}

static bool CheckStartupRegistration() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, STARTUP_REGISTRY_PATH, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS) {
        return false;
    }

    bool isRegistered = (RegQueryValueExW(hKey, MQTTTOASTER_REGVAL_NAME, NULL, NULL, NULL, NULL) == ERROR_SUCCESS);
    RegCloseKey(hKey);
    return isRegistered;
}

// TrayIcon 実装
static LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    TrayIcon* tray = reinterpret_cast<TrayIcon*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        const auto create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        tray = static_cast<TrayIcon*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(tray));
    }
    if (msg == WM_TRAYICON && lParam == WM_RBUTTONUP) {
        POINT pt;
        GetCursorPos(&pt);
        HMENU hMenu = LoadMenu(GetModuleHandle(nullptr), MAKEINTRESOURCE(IDR_TRAYMENU));
        HMENU hPopup = GetSubMenu(hMenu, 0);
        SetForegroundWindow(hwnd);

        // check start up registration
		CheckMenuItem(hPopup, ID_STARTUP, CheckStartupRegistration() ? MF_BYCOMMAND | MF_CHECKED : MF_BYCOMMAND | MF_UNCHECKED);

        int cmd = TrackPopupMenu(hPopup, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, NULL);
        DestroyMenu(hMenu);

        if (cmd == ID_EXIT) {
            spdlog::info("Exit selected from tray menu. Terminating...");
            PostQuitMessage(0);
        }
        else if (cmd == ID_STARTUP) {
            // startup registration toggle
			ToggleStartupRegistration();
        }
        else if (cmd == ID_SETTINGS && tray) {
            tray->OpenSettings(hwnd);
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool TrayIcon::Create(HINSTANCE hInstance, std::function<void(HWND)> settingsHandler) {
    settingsHandler_ = std::move(settingsHandler);
    WNDCLASSEXW wc = { 0 };
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"MQTT_Notification_TrayClass";

    RegisterClassExW(&wc);

    hwnd = CreateWindowExW(0, wc.lpszClassName, _CRT_WIDE(MQTTTOASTER_DEFAULT_TITLE), 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInstance, this);
    if (!hwnd) return false;

    HICON hIcon = (HICON)LoadImage(
        GetModuleHandle(nullptr),
        MAKEINTRESOURCE(IDI_TRAYICON),
        IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON),  // 16x16
        GetSystemMetrics(SM_CYSMICON),
        0);

    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = hIcon;
    wcscpy_s(nid.szTip, _CRT_WIDE(MQTTTOASTER_DEFAULT_TITLE));

    return Shell_NotifyIconW(NIM_ADD, &nid);
}

void TrayIcon::OpenSettings(HWND owner) const {
    if (settingsHandler_) {
        settingsHandler_(owner);
    }
}

void TrayIcon::Destroy() {
    if (hwnd) {
        Shell_NotifyIconW(NIM_DELETE, &nid);
        DestroyWindow(hwnd);
        hwnd = NULL;
    }
}

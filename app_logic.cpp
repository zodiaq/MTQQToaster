#include "app_logic.h"
#include <fstream>
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")

#include <nlohmann/json.hpp>
#include <spdlog/sinks/rotating_file_sink.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.UI.Notifications.h>

#define MQTTTOASTER_REGVAL_NAME L"MQTTToaster"
#define STARTUP_REGISTRY_PATH L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"

using json = nlohmann::json;
using namespace winrt;
using namespace Windows::Data::Xml::Dom;
using namespace Windows::UI::Notifications;

#define WM_TRAYICON (WM_USER + 1)
#define IDM_EXIT    1001

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
        spdlog::filename_t logPathStr(logPath.string());
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
			std::vector<std::string> topics;
			for (const auto& topic : j["mqtt_topics"])
            {
				if (topic.is_string()) {
					topics.push_back(topic.get<std::string>());
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

    // 2. std::filesystem でパスを解決（相対パスならモジュールディレクトリ基準）
    std::filesystem::path targetPath(wPath.c_str());
    if (targetPath.is_relative()) {
        targetPath = GetModuleDirectory() / targetPath;
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
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool TrayIcon::Create(HINSTANCE hInstance) {
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

void TrayIcon::Destroy() {
    if (hwnd) {
        Shell_NotifyIconW(NIM_DELETE, &nid);
        DestroyWindow(hwnd);
        hwnd = NULL;
    }
}
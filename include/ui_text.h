#pragma once

#include <windows.h>
#include <string>
#include <string_view>

enum class UiString {
    WindowTitle,
    Settings,
    Connection,
    Endpoint,
    User,
    Password,
    Subscriptions,
    Topic,
    Image,
    Browse,
    Update,
    New,
    Delete,
    Log,
    LogLevel,
    LogSize,
    LogFiles,
    RestartHint,
    Cancel,
    Save,
    HasImage,
    OperationFailed,
    EnterTopic,
    BrowseFailed,
    InvalidLog,
    NeedTopic,
    ImageMismatch,
    ImageCopyFailed,
    SaveFailed,
    MenuSettings,
    Startup,
    StartupSettings,
    Exit,
    DataInitFailed,
    Count
};

bool IsJapaneseUi(LANGID language = GetThreadUILanguage()) noexcept;
const wchar_t* UiText(UiString id, LANGID language = GetThreadUILanguage()) noexcept;
std::wstring LocalizeMqttError(std::string_view error);
bool LocalizeTrayMenu(HMENU menu, bool packaged, LANGID language = GetThreadUILanguage()) noexcept;

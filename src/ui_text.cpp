#include "ui_text.h"
#include "resource.h"
#include <array>
#include <winrt/base.h>

namespace {
struct Translation { const wchar_t* ja; const wchar_t* en; };
constexpr std::array translations{
    Translation{L"MTQQToaster 設定", L"MTQQToaster Settings"},
    Translation{L"設定", L"Settings"},
    Translation{L"MQTT 接続", L"MQTT connection"},
    Translation{L"接続先", L"Server"},
    Translation{L"ユーザー名", L"Username"},
    Translation{L"パスワード", L"Password"},
    Translation{L"購読トピック", L"Subscriptions"},
    Translation{L"トピック", L"Topic"},
    Translation{L"既定画像", L"Default image"},
    Translation{L"参照…", L"Browse…"},
    Translation{L"更新", L"Update"},
    Translation{L"<新規>", L"<New>"},
    Translation{L"削除", L"Delete"},
    Translation{L"ログ", L"Logging"},
    Translation{L"ログレベル", L"Log level"},
    Translation{L"最大サイズ MB", L"Max size (MB)"},
    Translation{L"保持数", L"Files to keep"},
    Translation{L"最大サイズと保持数の変更は、再起動後に反映されます。", L"Changes to size and file count take effect after restarting."},
    Translation{L"キャンセル", L"Cancel"},
    Translation{L"保存", L"Save"},
    Translation{L"  (画像あり)", L"  (with image)"},
    Translation{L"設定画面の操作に失敗しました。", L"The settings operation failed."},
    Translation{L"topicを入力してください。", L"Enter a topic."},
    Translation{L"画像選択ダイアログを開けませんでした。", L"Could not open the image picker."},
    Translation{L"ログサイズと保持数には有効な1以上の整数を入力してください。", L"Enter valid positive integers for log size and file count."},
    Translation{L"購読topicを1件以上登録してください。", L"Add at least one subscription."},
    Translation{L"topicの画像設定が不整合です。設定画面を開き直してください。", L"Topic image settings are inconsistent. Reopen Settings."},
    Translation{L"画像を設定フォルダーへコピーできませんでした。PNG、JPEG、GIFを選択してください。", L"Could not copy the image to the settings folder. Select a PNG, JPEG or GIF."},
    Translation{L"設定を保存できませんでした。", L"Could not save settings."},
    Translation{L"設定...", L"Settings..."},
    Translation{L"スタートアップに登録", L"Run at startup"},
    Translation{L"Windowsのスタートアップ設定...", L"Windows startup settings..."},
    Translation{L"終了", L"Exit"},
    Translation{L"アプリケーションデータフォルダーを初期化できませんでした。", L"Could not initialize the application data folder."},
};
static_assert(translations.size() == static_cast<size_t>(UiString::Count));
}

bool IsJapaneseUi(LANGID language) noexcept { return PRIMARYLANGID(language) == LANG_JAPANESE; }
bool LocalizeTrayMenu(HMENU menu, bool packaged, LANGID language) noexcept
{
    return ModifyMenuW(menu, ID_SETTINGS, MF_BYCOMMAND | MF_STRING, ID_SETTINGS, UiText(UiString::MenuSettings, language)) &&
        ModifyMenuW(menu, ID_STARTUP, MF_BYCOMMAND | MF_STRING, ID_STARTUP,
            UiText(packaged ? UiString::StartupSettings : UiString::Startup, language)) &&
        ModifyMenuW(menu, ID_EXIT, MF_BYCOMMAND | MF_STRING, ID_EXIT, UiText(UiString::Exit, language));
}
const wchar_t* UiText(UiString id, LANGID language) noexcept
{
    const auto index = static_cast<size_t>(id);
    if (index >= translations.size()) return L"";
    return IsJapaneseUi(language) ? translations[index].ja : translations[index].en;
}

std::wstring LocalizeMqttError(std::string_view error)
{
    if (IsJapaneseUi()) {
        if (error == "MQTT URL must use mqtt:// or mqtts://") return L"接続先には mqtt:// または mqtts:// を指定してください。";
        if (error == "MQTT URL must contain only a host and optional port") return L"接続先にはホスト名とポートだけを指定してください。";
        if (error == "Invalid IPv6 MQTT host") return L"IPv6アドレスの形式が正しくありません。";
        if (error == "Brackets require an IPv6 address") return L"角括弧の中にはIPv6アドレスを指定してください。";
        if (error == "Invalid MQTT port") return L"ポートは1～65535の整数で指定してください。";
        if (error == "Empty MQTT port") return L"ポート番号を入力してください。";
        if (error == "Invalid MQTT host") return L"ホスト名が正しくありません。";
        if (error == "Configure between 1 and 128 MQTT topics") return L"購読トピックは1～128件で指定してください。";
        if (error == "Invalid MQTT topic filter") return L"購読トピックの形式が正しくありません。ワイルドカードや文字数を確認してください。";
        if (error == "Invalid MQTT credentials (password requires username)") return L"認証情報が正しくありません。パスワードを使用する場合はユーザー名も入力してください。";
    }
    return std::wstring(winrt::to_hstring(error));
}

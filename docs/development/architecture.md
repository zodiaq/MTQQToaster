# 実装構成

[ドキュメント一覧](../README.md) · [現行仕様](../../SPECIFICATION.md)

## モジュールの責務

- [main.cpp](../../src/main.cpp)：STA、メッセージループ、設定共有、監視、MQTTワーカー、起動と終了。
- [app_logic.cpp](../../src/app_logic.cpp)／[app_logic.h](../../include/app_logic.h)：設定、保存先、画像、ログ、ペイロード、トースト、トレイ、スタートアップ。
- [settings_ui.cpp](../../src/settings_ui.cpp)：Win32ホスト＋OS内蔵XAML設定画面、入力操作、配置の保存・復元。
- [mqtt_client.cpp](../../src/mqtt_client.cpp)：入力検証、coreMQTTの接続・購読・受信、トピック照合。
- [mqtt_transport.cpp](../../src/mqtt_transport.cpp)：Winsock、Schannel、証明書検証、キャンセル可能な通信。[詳細](../mqtt-transport.md)。
- [resources](../../resources)：ダイアログ、アイコン、マニフェスト。
- [tests](../../tests)：ロジック、MQTT、ローカル通信テスト。[実行手順](testing.md)。

## スレッドと設定共有

UIはSTA、MQTTの`std::jthread`はMTAをそれぞれ初期化します。ワーカーは接続単位で設定スナップショットと通信オブジェクトを保持します。

共有設定は`SRWLOCK`をRAIIで取得してコピー・更新し、通信やファイル操作はロック外で実行します。設定保存・再読込は再接続フラグを立て、ワーカーは停止要求と併せて観測します。

## 監視と終了

`StorageFileQueryResult.ContentsChanged`はUIスレッドへメッセージを送り、UI側で更新時刻を確認して再読込します。監視初期化の`IAsyncAction`は`App`が保持します。

終了時は停止フラグで新たな購読を防ぎ、購読を解除して初期化操作をキャンセルします。STAの継続が完了するまでメッセージを処理し、WinRTの解放後にapartmentを終了します。MQTTワーカーにも停止を要求して終了を待ちます。

## 実装上の注意

MQTT／JSONはUTF-8、Windows APIはUTF-16です。トーストXMLはDOMで構築します。C ABIの通信コールバックやWin32コールバックの外へ例外を漏らさない構成です。

タイトルの扱いには[制約](../reference/payload.md)があります。設定のJSON読込とUI入力検証も同一ではありません。改善案は現行動作の説明と混ぜず、[判断記録](../decisions/README.md)へ記載します。

## 設定画面のXAMLホスト

設定画面はC++/WinRTからOS内蔵のWindows.UI.Xamlを生成し、DesktopWindowXamlSourceでWin32ウィンドウへ埋め込みます。C#、WinUI 3、Windows App SDK、XAMLコンパイラー、カスタムWinRT型・IDLは使用しません。配置と保存処理の接続はSettingsUi内にまとめます。

XAML初期化中はUI Automation／COMからネストしたウィンドウ通知が到達する場合があります。トレイメニューの表示処理はスコープ付きガードで再入を防ぎ、処理中の重複通知は最初の表示へまとめます。単一STAであってもウィンドウ通知が非再入であるとは仮定しません。

UIスレッドのWindowsXamlManagerとislandは初回表示時に生成し、アプリ終了まで保持します。閉じる操作は非表示にし、再表示時は設定スナップショットからフォームを作り直します。最終islandを途中で破棄する構成では設定監視の回帰を検出したためです。保存・キャンセルはイベントを抜けてから非表示にします。

終了時はイベント購読とXAML内容、island、親HWNDを解放し、WindowsXamlManager.Close後のSTAメッセージを処理してからCOMを終了します。詳細な判断と制約は[採用記録](../decisions/0002-os-xaml-settings.md)を参照してください。

Windows 11の設定ホストではDWMのキャプション色対応を確認し、非クライアント上端をクライアント領域へ拡張します。44 DIPのヘッダーにタイトルを描き、XAML子ウィンドウはその下へ配置します。DwmDefWindowProcで標準キャプションボタンを処理し、ヘッダーのヒットテストをHTCAPTIONへ返します。Windows 10／ハイコントラストでは標準枠に戻します。

表示文言は[ui_text.cpp](../../src/ui_text.cpp)に日英の対で管理します。GetThreadUILanguageの主言語が日本語なら日本語、それ以外は英語です。設定UIとトレイメニューで同じ判定を使い、MSIXのスタートアップ項目も翻訳します。MQTT検証の既知エラーはUI側で日本語化し、通信層の例外・ログの識別は維持します。

設定ホストはWS_CAPTIONとWS_SYSMENUのみを持つ固定サイズです。700×650 DIPのフォームをViewboxで包み、小さい作業領域では縮小します。購読一覧の末尾は設定データに含めない新規項目で、削除では無視し、更新では追加します。プレビュー画像はWICで128×96以内のPBGRAへ縮小してWriteableBitmapに渡します。相対パスは設定ディレクトリを基準とし、選択・参照・画像欄のフォーカス移動で読み直します。

トレイメニューは同じSTAとWindowsXamlManager上の別islandです。HWNDとislandは非表示でも保持し、終了時に購読解除・解放します。ボタンのClickはWM_COMMANDへ転送し、イベントを抜けてから非表示・コマンド実行します。非アクティブ化とEscで閉じ、上下キー・Tab・Enterを扱います。ハイコントラストと初期化失敗では従来のTrackPopupMenuへ戻します。角丸は[Microsoftのカスタムメニュー向けDWM手順](https://learn.microsoft.com/en-us/windows/apps/desktop/modernize/ui/apply-rounded-corners#example-4--rounding-the-corners-of-a-menu-with-a-small-radius---c)を使用します。シェル内部のメニューAPIや追加ランタイムは使用しません。

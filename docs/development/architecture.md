# 実装構成

[ドキュメント一覧](../README.md) · [現行仕様](../../SPECIFICATION.md)

## モジュールの責務

- [main.cpp](../../src/main.cpp)：STA、メッセージループ、設定共有、監視、MQTTワーカー、起動と終了。
- [app_logic.cpp](../../src/app_logic.cpp)／[app_logic.h](../../include/app_logic.h)：設定、保存先、画像、ログ、ペイロード、トースト、トレイ、スタートアップ。
- [settings_ui.cpp](../../src/settings_ui.cpp)：Win32設定画面、入力操作、配置の保存・復元。
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

# MTQQToaster 現行仕様

[README](README.md) · [ドキュメント一覧](docs/README.md)

現在の動作と制約を記述します。操作は[マニュアル](docs/user-guide.md)、内部構造は[実装構成](docs/development/architecture.md)、提案・採用理由は[判断記録](docs/decisions/README.md)を参照してください。

## 対応環境

Windows 10 version 1809以降／Windows 11のx64向けトレイ常駐EXEです。Win32とC++/WinRTを使用し、現行ビルド・配布構成にUWP、WinUI、Windows App SDK、MSIX、IDLは含みません。

## 通知

受信したUTF-8のJSONまたはプレーンテキストを解析し、Windows標準のトーストAPIへ渡します。本文・画像・タイトルの扱いは[ペイロード仕様](docs/reference/payload.md)を正本とします。

画像なしは`ToastText01`、画像ありは`ToastImageAndText01`を使用します。実表示はWindows側の通知設定や通知元の登録条件にも依存します。

## MQTTとTLS

MQTT 3.1.1のclean sessionで接続し、複数トピックをQoS 0で購読します。`+`／`#`に対応します。keep-aliveは60秒。切断後は5秒待って再接続・再購読し、設定更新と終了要求で待機を中断します。

`mqtt`／`mqtts`に対応します。TLS 1.2以降でWindowsの信頼ストアによる証明書チェーン、サーバー用途、有効期限、失効状態、接続先名を検証します。失効確認不能も接続失敗です。検証を無効にする設定はありません。

購読数は1～128件、受信MQTTパケット全体は256 KiBまで。超過、不正応答、購読拒否では切断・再接続します。入力制約は[設定仕様](docs/reference/configuration.md)、待機時間とバッファ管理は[通信層](docs/mqtt-transport.md)を参照してください。

MQTT 5、WebSocket、永続セッション、クライアント証明書認証は対象外です。

## 設定・常駐・ログ

Win32設定画面から接続・購読・画像・ログを編集し、ユーザー別の設定ファイルへ保存します。画面を閉じても常駐し、トレイの終了操作で停止します。画面配置は別ファイルへ保存します。自動起動はユーザーのログオン時の起動登録です。

設定ファイルをWinRTのファイルクエリで監視します。更新検出後に読み込み、解析失敗時は共有設定を置き換えません。成功時にはMQTT再接続を要求します。JSON読込とMQTT入力検証は別処理であり、読込成功が接続成功を保証するものではありません。

ログレベルは実行中に更新し、ログのサイズ・保持数は起動時に適用します。保存形式、初期値、移行処理は[設定仕様](docs/reference/configuration.md)を参照してください。

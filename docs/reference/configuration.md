# 設定ファイル リファレンス

[ドキュメント一覧](../README.md) · [現行仕様](../../SPECIFICATION.md)

通常は[設定画面](../user-guide.md)を使用します。実例は[config.sample.json](../../config.sample.json)を参照してください。

## 項目と内部既定値

既定値は`AppConfig`の値です。配布サンプルの値とは異なります。

- `mqtt_url`：文字列。既定`mqtt://127.0.0.1:1883`。
- `mqtt_topics`：`topic`と`image`を持つオブジェクトの配列。既定`[{"topic":"mqtt_toaster/#","image":""}]`。互換用に文字列配列も読めます。保存時はオブジェクト形式です。
- `mqtt_user`、`mqtt_pass`：文字列。既定は空。平文のJSONとして保存します。
- `log_level`：文字列。既定`INFO`。`TRACE`、`DEBUG`、`INFO`、`WARN`、`ERROR`、`NONE`。
- `log_max_size_mb`：整数。既定5。1ファイルの最大サイズ（MiB単位）。
- `log_max_files`：整数。既定3。ローテーションするログの保持世代数。

設定画面はサイズ・保持数に正の整数を要求します。ログレベルは実行中に反映し、サイズ・保持数の変更には再起動が必要です。

## MQTT入力

URLは`mqtt://host[:port]`または`mqtts://host[:port]`。省略時のポートは1883／8883。IPv6は`mqtts://[::1]:8883`のように角括弧で囲みます。IDNはWindows APIでASCIIへ変換します。URLに認証情報やパスを含めません。

購読は1～128件。フィルターはUTF-8で65535 bytes以内。`+`／`#`はレベル境界規則に従います。ユーザー名・パスワードは各65535 bytes以内で、パスワード指定時はユーザー名が必要です。

トピックの`image`はアプリがコピーした`topic-image-*`のファイル名です。読み込みではディレクトリ付きや管理接頭辞のない値を採用しません。画像は設定画面から登録してください。

## 保存先と初回移行

通常は`%LOCALAPPDATA%\MTQQToaster`を使用します。環境変数を取得できない場合はEXEのディレクトリへフォールバックします。

`ApplicationData.Current()`が利用可能なら`LocalFolder`と`LocalCacheFolder`を使う分岐もありますが、現行リポジトリにMSIXのビルド・配布構成はありません。

保存先に`config.json`がない場合だけ、EXE横の`config.json`を優先し、なければ`config.sample.json`をコピーします。両方なければ起動時には作成せず、内部既定値を使います。設定画面で保存すると作成されます。

配置情報は`ui-state.json`、管理画像は`topic-image-*`、ペイロード画像の配置先は`ToastAssets/`、ログはキャッシュ側の`Logs/app.log`です。通常は設定とキャッシュの基準フォルダーが同じです。

## 読み込みの互換動作

欠損項目は内部既定値を使います。空または有効要素のないトピック配列も既定値を残します。読込は設定画面と同一の厳格な入力検証ではありません。外部編集後はログで再読込と接続結果を確認してください。

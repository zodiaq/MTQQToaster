# テスト手順

[ドキュメント一覧](../README.md) · [ビルド環境](build.md) · [過去の結果](../validation/README.md)

## 通常のテスト

リポジトリルートで実行します。

```powershell
cmake --preset x64-debug
cmake --build --preset x64-debug --parallel
ctest --preset x64-debug
```

Releaseでは`x64-release`に置き換えます。各コマンドの終了コードを確認し、構成・ビルド失敗後に古いEXEを実行して成功扱いにしません。テスト0件も成功として扱いません。

GoogleTestで設定・ペイロード・画面配置のロジックと、MQTTのURL、UTF-8、ワイルドカード、長さ上限、分割送受信、認証・購読拒否、タイムアウト、keep-alive、停止、コールバック例外を確認します。

## UIと設定監視

対話可能なWindowsデスクトップで、UIの回帰テストを実行できます。

```powershell
$process = Start-Process .\out\build\windows-x64\Release\MTQQToaster.exe --ui-smoke-test -PassThru -Wait
$process.ExitCode
```

ウィンドウなしの起動、設定画面の読み込み・中央配置・スクロール量・ログ3列配置、位置サイズの保存・復元・初期化、topicの追加・更新・削除、画像解除、保存・キャンセル、閉じた後の常駐を検証します。専用の一時フォルダーで設定監視による再読み込みと監視初期化中の終了も検証します。成功時の終了コードは0、失敗は非0です。診断とテスト用の位置情報はEXEと同じフォルダーの`ui-smoke.log`、`ui-smoke-state.json`へ記録します。実設定にはアクセスせず、MQTT接続やトレイ登録も行いません。設定画面が一時的に表示されるため、通常のCTestとは別に実行します。

`--ui-preview`では、実設定やネットワークを使わずにサンプル設定の画面を手動確認できます。保存はメモリ内だけに適用され、画面を閉じると終了します。

`--ui-preview-ja`／`--ui-preview-en`と`--ui-smoke-test-ja`／`--ui-smoke-test-en`は、試験プロセスだけの表示言語を固定します。Windowsの言語設定や実設定ファイルは変更しません。通常起動には言語の上書きは適用しません。UIスモークは初回XAMLレイアウトが完了するまで最大5秒待ってから、配置・ログ列の整列を判定します。

設定画面はOS内蔵XAMLを使用します。スモークテストの編集操作はSettingsUiの検証用メソッドから実際のXAML入力欄と共通コマンド処理を操作し、保存コールバックの結果を検査します。実クリック、Tab／Shift+Tab、IME、縮小時のスクロール、テーマ、DPIは別途手動確認します。

OS内蔵XAMLの初期化・標準部品生成・3回のisland生成／破棄だけを分離して検証する場合は、次を実行します。この試験は設定監視との共存を保証しません。

```powershell
cmake --preset x64-release -DMTQQTOASTER_XAML_PROBE=ON
cmake --build --preset x64-release --target MTQQToasterXamlProbe
.\out\build\windows-x64\Release\MTQQToasterXamlProbe.exe
```

試験後は`-DMTQQTOASTER_XAML_PROBE=OFF`で通常の構成に戻せます。実設定・MQTT通信・トレイ登録は使いません。追加ランタイムの要否は、EXEの直接依存に加えてXAML表示後の読み込みモジュールとクリーン環境で確認します。

## ローカルTCP／TLS結合テスト

PowerShell 7.5以降が必要です（配布先には不要）。

```powershell
cmake --preset x64-release -DMTQQTOASTER_TRANSPORT_TESTS=ON
cmake --build --preset x64-release --parallel
ctest --preset x64-release
```

オプションはDebug／Release共通のキャッシュに残ります。通常構成へ戻すには`cmake --preset x64-release -DMTQQTOASTER_TRANSPORT_TESTS=OFF`を実行します。

.NET SslStreamのループバックサーバーでTCP／TLSのバイナリ往復、正常証明書、未知CA、名前不一致、期限切れ、TLS初期化中の停止、日本語topicの受信、再接続・再購読を確認します。fixtureはフル機能のブローカーではありません。

クライアントの信頼はメモリ内の専用チェーンエンジンで構成し、ユーザーの証明書ストアは変更しません。サーバーは一時鍵コンテナーを使用し、証明書の破棄時に削除します。

## 実機確認と記録

`--tray-smoke-test`は実際のトレイ通知経路から、XAMLの初期化中に通知が再入する条件を検証します。UIスレッドだけのCBTフックでウィンドウ生成中に通知を再送し、フックは試験後に解除します。`--tray-live-test`は手動右クリック用のトレイを起動します。両モードとも一時設定と設定監視を使用し、実設定・MQTT通信は使いません。スタートアップ項目は登録を変更しません。「設定」は試験用画面を開き、「終了」は試験を終了します。

`--menu-preview-ja`／`--menu-preview-en`で実設定とスタートアップ登録に触れず、メニュー単独を表示できます。検証用にタスクバーへ表示し、検証ツールのフォーカス移動でも閉じない設定です。通常のメニューとは非アクティブ化の動作が異なります。「設定」はプレビュー設定画面を開き、それ以外はプレビューを終了します。通常の`--ui-smoke-test-ja`／`--ui-smoke-test-en`では固定スタイル、新規項目の追加と削除防止、メニューの再表示・終了、設定監視を検証します。

トレイアイコンはWindowsモードをダーク／ライトで切り替え、起動時と常駐中の両方で白／黒になることを確認します。カスタムモードでWindowsとアプリのモードを逆に設定し、タスクバー側に従うことも確認します。GoogleTestではEXEに埋め込んだ白・黒アイコンを各トレイサイズで読み込めることを検証します。

実ブローカー、独自CA、VPN／プロキシ、TLS再ネゴシエーション、トーストの実表示は必要に応じて別途確認します。単体テストだけで実環境の動作を断定しません。

文書のみの変更ではリンク、例の構文、実装との対応、差分を確認し、不要なアプリ起動や通信は行いません。試験結果は日付付きの[検証記録](../validation/README.md)へ記載します。

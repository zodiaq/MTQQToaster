---
name: cpp-winrt-development
description: Develop and debug MTQQToaster C++/WinRT and Win32 features, including toast notifications, config watching, MQTT workers, and MSVC/CMake/vcpkg builds. Use for related implementation, troubleshooting, and code review in this repository.
---

# C++/WinRT 開発

MTQQToasterの既存構成に沿って、変更対象のスレッド、所有権、終了処理を追跡してから実装する。

## 変更箇所を決める

1. ルートの`AGENTS.md`と変更対象のコードを読み、呼び出し元とテストを確認する。
2. 通知、apartment、非同期処理、監視、Win32リソースを扱う場合は [windows-runtime.md](references/windows-runtime.md) を読む。
3. ビルド、依存関係、コンパイラ診断、テストを扱う場合は [build-and-test.md](references/build-and-test.md) を読む。
4. SDKのAPIを追加・変更する際は、対象Windows SDK、OS要件、デスクトップアプリでの利用条件をMicrosoft Learnで確認する。

## 実装時の判断

- `main.cpp`はWin32メッセージループと`std::jthread`を持つ。UIスレッドとMQTTワーカーを分け、スレッドをまたぐWinRTオブジェクトのapartment制約を確認する。
- 設定監視は現在`Windows.Storage.Search.StorageFileQueryResult`の`ContentsChanged`を使用する。仕様書の`ReadDirectoryChangesW`という記述だけで実装方式を変更しない。
- 設定は別の`AppConfig`へ解析・検証してから共有状態へ公開し、失敗時は最後の有効な設定を維持する。利用側はロック下でコピーし、ロック解放後に通信や通知を行う。
- 設定変更とMQTT再接続の競合、イベントの重複、エディタの一時ファイルを使った保存、終了中のイベント到着を変更範囲に応じて扱う。
- `IAsyncAction`等の結果と例外を観測する所有者を決める。STAでの`.get()`、一時オブジェクトを参照するコルーチン、所有者を失う非同期ラムダを避ける。
- 通知では現在の`Windows.UI.Notifications`を前提に調査する。表示の失敗はHRESULT、アプリIDと登録、画像URI、OSの通知設定から切り分ける。
- 純粋な変換処理はWindowsの表示・登録処理から分離し、`MTQQToasterLib`とGoogleTestで検証できる形を優先する。

## 確認する

- 変更した解析・設定・パス処理には、欠損キー、不正な型、日本語、必要に応じて不正UTF-8やURIなどの意味のあるケースを確認する。
- 非同期・監視・再接続の変更では、停止要求からスレッド終了、購読解除、コールバック完了、オブジェクト解放までを追う。STAへの復帰が必要な処理をメッセージループ停止後に待たない。
- コードを変更した場合はビルドして該当テストを実行する。リリース設定や依存関係を変えた場合はReleaseも確認する。環境不足なら不足項目と実行できなかったコマンドを報告する。
- 通知が実際に表示されたことや監視が停止したことは、単体テストの成功だけから断定しない。実施した実機確認と未確認項目を分けて報告する。

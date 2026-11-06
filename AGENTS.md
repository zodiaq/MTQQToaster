# MTQQToaster 開発指示

このリポジトリでは、WindowsデスクトップとC++/WinRTを扱う開発者として作業する。
ユーザーへの説明は日本語を基本とし、既存コードの命名と書式に合わせる。

## プロジェクトの前提

- C++23、MSVC、Windows SDK、CMake、Ninja、vcpkgを使用する。
- Win32のトレイ常駐EXEであり、C++/WinRTでWindows APIを利用する。
- 現在の構成にはUWP、WinUI、Windows App SDK、MSIX、IDLはない。変更が必要な依頼では導入理由と影響を示す。
- `CMakeLists.txt`、`CMakePresets.json`、`vcpkg.json`でビルドと依存関係を確認する。
- `README.md`、`SPECIFICATION.md`は意図を把握する資料。実装と食い違う場合は差を説明し、今回の依頼に沿って判断する。

## コードの所在

- `src/main.cpp`: 起動、STAメッセージループ、MQTTワーカー、WinRT設定監視、終了処理。
- `include/app_logic.h` / `src/app_logic.cpp`: 設定、ペイロード変換、ログ、通知、画像パス、トレイ、スタートアップ登録。
- `tests/test_main.cpp`: GoogleTestによるペイロード変換と設定読み込みのテスト。
- `resources/app.rc` / `resources/resource.h` / `resources/tray.ico`: Windowsリソース。
- `config.sample.json`: 設定例。実行時の設定とログはEXEのディレクトリを基準とする。

## 実装の基準

- C++/WinRT、Windows連携、ビルドに関わる変更には [cpp-winrt-development](.agents/skills/cpp-winrt-development/SKILL.md) を使い、必要な参照資料だけを読む。
- WinRTを呼ぶスレッドごとにCOM/WinRT初期化とapartmentを確認する。起動スレッドの初期化をワーカーへ引き継げると考えない。
- コルーチン、コールバック、イベント購読には所有者と停止手順を持たせ、終了中の再登録と解放後のアクセスを防ぐ。
- `SRWLOCK`の保護対象を明確にし、例外安全な解放と設定スナップショットを使う。ロック中のI/O、待機、`co_await`を避ける。
- Win32リソースは種類に合うRAIIで管理し、戻り値を確認する。C ABIコールバックの外へC++例外を漏らさない。
- MQTT/JSONのUTF-8とWindows APIのUTF-16を明示的に変換し、XMLにはDOMで値を設定する。
- MQTTペイロードと設定を外部入力として扱う。認証情報をログへ追加せず、TLS検証の無効化を解決策にしない。
- 依存ライブラリは既存のvcpkgマニフェストで管理し、静的CRTと`x64-windows-static`の整合を保つ。

## 検証と完了報告

- ビルド手順は [build-and-test.md](.agents/skills/cpp-winrt-development/references/build-and-test.md) に従う。生成物やキャッシュを直接編集しない。
- ロジック変更には該当するGoogleTestを追加・更新し、GUI、通知、監視、終了処理は必要なWindows実機確認を行う。
- ドキュメントや指示だけの変更では、参照先、構文、差分を確認する。不要なアプリ起動やネットワーク接続を検証に含めない。
- 既存の失敗を今回の変更による失敗と区別し、テストの削除や期待値の緩和で隠さない。
- 変更内容、実施した検証、未実施の検証と理由を報告する。

# MTQQToaster

MQTTで受信したメッセージをWindowsのトースト通知として表示する、トレイ常駐アプリです。Home Assistantなどからの通知をデスクトップで受け取れます。

Windows 10 version 1809以降／Windows 11（x64）を対象とします。MQTT 3.1.1、TLS、複数トピックの購読、画像付き通知、ログオン時の自動起動に対応します。

## はじめに

1. [ビルド手順](docs/development/build.md)に従ってEXEを作成します。
2. `out/build/windows-x64/Release/MTQQToaster.exe`を起動します。
3. トレイの「設定...」で接続先と購読トピックを保存します。
4. [メッセージ形式](docs/reference/payload.md)に従って通知を送信します。

詳しい操作は[操作マニュアル](docs/user-guide.md)を参照してください。

## ドキュメント

- **使う** — [操作マニュアル](docs/user-guide.md)：初期設定、画像、自動起動、ログ、困ったとき。
- **動作を確認する** — [現行仕様](SPECIFICATION.md)、[設定リファレンス](docs/reference/configuration.md)、[メッセージ形式](docs/reference/payload.md)。
- **開発する** — [ビルド](docs/development/build.md)、[テスト](docs/development/testing.md)、[実装構成](docs/development/architecture.md)、[MQTT通信層](docs/mqtt-transport.md)。
- **経緯を調べる** — [設計判断・検討記録](docs/decisions/README.md)、[検証記録](docs/validation/README.md)。
- **文書を更新する** — [構成と管理方針](docs/README.md)。
- **AIと開発する** — [リポジトリ指示](AGENTS.md)、[C++/WinRT開発スキル](.agents/skills/cpp-winrt-development/SKILL.md)。

## 技術構成・ライセンス

C++23／MSVC、Win32、Windows SDKのC++/WinRT、CMake、vcpkgを使用します。MQTTはcoreMQTT、TLSはWindows標準のSchannel、JSONはnlohmann/json、ログはspdlog、単体テストはGoogleTestです。依存関係は[vcpkg.json](vcpkg.json)と[overlay port](ports/coremqtt/portfile.cmake)で管理します。

静的CRTでビルドし、追加のWindows App Runtime、.NET、OpenSSL、VC++ Redistributableは不要です。

本プロジェクトは[MIT License](LICENSE.txt)です。配布時には依存ライブラリのライセンス文書も含めてください。

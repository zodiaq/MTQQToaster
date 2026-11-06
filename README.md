# MTQQToaster

MTQQToaster is a simple MQTT Windows client that allows you to receive messages using the MQTT protocol.
It is designed to be lightweight and easy to use, making it ideal for the scenarios where you want to receive notifications from Home Assistant or other MQTT brokers.

## Features
- Connect to an MQTT broker
 - TLS/SSL support for secure connections
- Subscribe to topics and receive messages
- Pop up toast notification for incoming messages
- Automatically start on system boot

## Installation

### ビルド環境の準備（Windows x64）

Visual Studio 2026 Community をインストールし、**「C++によるデスクトップ開発」**を選択してください。
推奨コンポーネントのMSVC x64/x86、Windows 11 SDK、**「Windows用C++ CMakeツール」**、**「vcpkgパッケージマネージャー」**を含めます。
Visual Studio Installerで、このリポジトリの[.vsconfig](.vsconfig)をインポートしても準備できます。

CMake、Ninja、vcpkg、C++/WinRTを別途インストールする必要はありません。
ライブラリは`vcpkg.json`に従って最初のCMake構成時に自動取得されます。初回はインターネット接続とダウンロード・ビルドの時間が必要です。
vcpkgのclone、bootstrap、`vcpkg integrate install`、`VCPKG_ROOT`の手動設定は不要です。

### ビルドとテスト

スタートメニューから **Developer PowerShell for VS 2026** を開き、リポジトリのディレクトリで実行します。
VS付属のCMake（4.2以降）を利用できます。CMakeがPATHにある場合は通常のPowerShellでも同じコマンドを使えます。

```powershell
cmake -B build -S .
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure --no-tests=error
```

EXEは`build/Release/MTQQToaster.exe`です。`config.sample.json`をEXEと同じディレクトリへ`config.json`としてコピーし、設定してから起動します。
Debugの場合は、ビルドとテストの`Release`を`Debug`に置き換えてください。
アプリと依存ライブラリはx64・静的CRT（Release: `/MT`、Debug: `/MTd`）でビルドします。

プリセットを利用する場合（DebugとReleaseで構成・依存ライブラリを共有します）:

```powershell
cmake --preset x64-release
cmake --build --preset x64-release --parallel
ctest --preset x64-release
```

Debug用には`x64-debug`を指定します。プリセットのEXE出力先は`out/build/windows-x64/Release/`または`Debug/`です。

### 以前の構成で失敗していた場合

古いtoolchainやgeneratorの設定が`build`に残っている場合は、一度だけキャッシュを作り直します。

```powershell
cmake --fresh -B build -S .
```

`cmake --version`が4.2未満の場合はVS2026のDeveloper PowerShellで実行し直してください。
vcpkgが見つからない場合は、Visual Studio Installerで上記コンポーネントを確認します。
独自のvcpkgを使う場合は`VCPKG_ROOT`、または`-DCMAKE_TOOLCHAIN_FILE=.../scripts/buildsystems/vcpkg.cmake`で明示できます。変更時は新しいビルドディレクトリか`--fresh`を使ってください。
以前に設定した`VCPKG_ROOT`からVS付属vcpkgへ戻す場合は、そのシェルで`$env:VCPKG_ROOT = $null`を実行してから`cmake --fresh -B build -S .`を実行します。

参考: [VS2026のC++コンポーネント](https://learn.microsoft.com/en-us/visualstudio/install/workload-component-id-vs-community?view=vs-2026)、[vcpkgのCMake連携](https://learn.microsoft.com/en-us/vcpkg/users/buildsystems/cmake-integration)、[CMakeのVS2026対応](https://cmake.org/cmake/help/latest/generator/Visual%20Studio%2018%202026.html)。

## Configuration
Edit the `config.json` file to configure the MQTT broker settings, topics to subscribe to, and other options. The configuration file should be placed in the same directory as the executable.
- `mqtt_url`: The URL of the MQTT broker (e.g., mqtt://broker.hivemq.com:1883)
- `mqtt_topics`: A list of topics to subscribe to. Wildcards are supported (e.g., ["home/+/temperature"])
- `mqtt_user`, `mqtt_pass`: Optional username and password for the MQTT broker
- `log_level`: Logging level (e.g., "INFO", "DEBUG", "ERROR")
- `log_max_size_mb`, `log_max_files`: Logging configuration for file size and number of log rotation files

## Dependencies

- Visual Studio 2026 Community with Desktop development with C++ (MSVC, Windows SDK, CMake and vcpkg included)
- Windows SDK C++/WinRT headers (for toast notifications and others)
- Automatically restored via vcpkg: mosquitto, spdlog, nlohmann/json and GoogleTest

## License
MTQQToaster is licensed under the MIT License. See the LICENSE file for more information.
For the licenses of the dependencies used in this project, please refer to their respective documentation.

## AI-assisted development

[AGENTS.md](AGENTS.md) contains repository instructions for C++23, Win32, and C++/WinRT development.
The repository skill [cpp-winrt-development](.agents/skills/cpp-winrt-development/SKILL.md) covers implementation, troubleshooting, and review, with references for [building and testing](.agents/skills/cpp-winrt-development/references/build-and-test.md) and [Windows runtime behavior](.agents/skills/cpp-winrt-development/references/windows-runtime.md).

Codex discovers project skills in `.agents/skills/`. Invoke it explicitly with `$cpp-winrt-development`, for example:

```text
$cpp-winrt-development 設定監視の終了処理を確認して、必要な修正と検証を行って
```

Start a new session in this repository to load the new repository instructions. If the skill does not appear, restart Codex.
See the official documentation for [AGENTS.md](https://learn.chatgpt.com/docs/agent-configuration/agents-md) and [skills](https://learn.chatgpt.com/docs/build-skills).

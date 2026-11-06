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

WinUI 3用のWindows App SDKはMicrosoftのWinApp CLIで復元します。初回だけ次を実行してください。

```powershell
winget install --id Microsoft.winappcli --source winget
winapp restore
```

`winapp.yaml`がWindows App SDKと投影ヘッダーの版を固定します。CMake構成時に`.winapp`がなければ`winapp restore`を自動実行します。

### ビルドとテスト

スタートメニューから **Developer PowerShell for VS 2026** を開き、リポジトリのディレクトリで実行します。
VS付属のCMake（4.2以降）を利用できます。CMakeがPATHにある場合は通常のPowerShellでも同じコマンドを使えます。

```powershell
cmake -B build -S .
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure --no-tests=error
```

EXEは`build/Release/MTQQToaster.exe`です。初回起動時にユーザー別のアプリデータフォルダーへ設定ファイルを作成します。トレイアイコンを右クリックして「設定...」を選び、MQTT接続とログ設定を入力してください。
Debugの場合は、ビルドとテストの`Release`を`Debug`に置き換えてください。
アプリと依存ライブラリはx64・静的CRT（Release: `/MT`、Debug: `/MTd`）でビルドします。
unpackaged実行ではWindows App SDK Runtimeが必要です。`winapp restore`は開発機へ対応するランタイムも導入します。配布時はMicrosoftのWindows App Runtime installerをセットアップへ含めてください。

プリセットを利用する場合（DebugとReleaseで構成・依存ライブラリを共有します）:

```powershell
cmake --preset x64-release
cmake --build --preset x64-release --parallel
ctest --preset x64-release
```

Debug用には`x64-debug`を指定します。プリセットのEXE出力先は`out/build/windows-x64/Release/`または`Debug/`です。

対話可能なWindowsデスクトップで、UIの回帰テストを実行できます。

```powershell
$process = Start-Process .\out\build\windows-x64\Release\MTQQToaster.exe --ui-smoke-test -PassThru -Wait
$process.ExitCode
```

ウィンドウなしの起動、設定画面の読み込み・中央配置・スクロール量・ログ3列配置、位置サイズの保存・復元・初期化、閉じた後の常駐、正常終了を検証します。成功時の終了コードは0、失敗は非0です。診断とテスト用の位置情報はEXEと同じフォルダーの`ui-smoke.log`、`ui-smoke-state.json`へ記録します。実設定にはアクセスせず、MQTT接続やトレイ登録も行いません。設定画面が一時的に表示されるため、通常のCTestとは別に実行します。

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
Use **Settings...** in the tray menu to configure the MQTT broker, subscribed topics, and logging. The application persists these values in its per-user application data directory; direct JSON editing is not required.

The settings window uses WinUI 3. The tray menu uses the native Win32 menu, and the notification-area icon uses `Shell_NotifyIcon`. Closing the settings window leaves the application running; use **Exit** in the tray menu to terminate it.

設定画面は初回、マウスがあるモニターの作業領域中央に、内容に合わせたサイズで表示します。位置・サイズ（最大化状態を含む）は閉じるときに設定フォルダーの`ui-state.json`へ保存し、次回起動後も復元します。**Shiftを押しながらトレイの「設定...」を選ぶと位置・サイズを初期化**します。設定画面が開いている場合もリセットできます。画面より内容が大きい場合や手動で縮小した場合はスクロールできます。ログレベル・最大サイズ・保持数は横並びです。

For an unpackaged build, the files are stored under `%LOCALAPPDATA%\MTQQToaster`. An MSIX-packaged build uses the package-managed `LocalFolder` and `LocalCacheFolder`. On first launch, an existing `config.json` beside the executable is imported for compatibility.

- `mqtt_url`: The URL of the MQTT broker (e.g., mqtt://broker.hivemq.com:1883)
- `mqtt_topics`: A list of topic objects. Each topic can have one application-managed default toast image. Payload images take precedence.
- `mqtt_user`, `mqtt_pass`: Optional username and password for the MQTT broker
- `log_level`: Logging level (e.g., "INFO", "DEBUG", "ERROR")
- `log_max_size_mb`, `log_max_files`: Logging configuration for file size and number of log rotation files

Runtime files are separated by purpose:

- `config.json`: persistent application data
- `topic-image-*`: per-topic default images copied beside `config.json`; unreferenced managed images are removed
- `Logs/app.log`: cache data that can be recreated

## Dependencies

- Visual Studio 2026 Community with Desktop development with C++ (MSVC, Windows SDK, CMake and vcpkg included)
- Windows SDK C++/WinRT headers (for toast notifications and others)
- Windows App SDK / WinUI 3 (restored by WinApp CLI according to `winapp.yaml`)
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

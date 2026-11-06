# ビルド手順

[ドキュメント一覧](../README.md) · [テスト手順](testing.md)

通常はプリセットを使用し、生成先を`out/build/windows-x64`に統一します。プリセットなしの手順も互換用に記載します。

## ビルド環境の準備（Windows x64）

Visual Studio 2026 Community をインストールし、**「C++によるデスクトップ開発」**を選択してください。
推奨コンポーネントのMSVC x64/x86、Windows 11 SDK、**「Windows用C++ CMakeツール」**、**「vcpkgパッケージマネージャー」**を含めます。
Visual Studio Installerで、このリポジトリの[.vsconfig](../../.vsconfig)をインポートしても準備できます。

CMake、Ninja、vcpkg、C++/WinRTを別途インストールする必要はありません。
ライブラリは`vcpkg.json`に従って最初のCMake構成時に自動取得されます。初回はインターネット接続とダウンロード・ビルドの時間が必要です。
vcpkgのclone、bootstrap、`vcpkg integrate install`、`VCPKG_ROOT`の手動設定は不要です。

## ビルドとテスト

スタートメニューから **Developer PowerShell for VS 2026** を開き、リポジトリのディレクトリで実行します。
VS付属のCMake（4.2以降）を利用できます。CMakeがPATHにある場合は通常のPowerShellでも同じコマンドを使えます。

DebugとReleaseで構成・依存ライブラリを共有します。

```powershell
cmake --preset x64-release
cmake --build --preset x64-release --parallel
ctest --preset x64-release
```

Debug用には`x64-debug`を指定します。プリセットのEXE出力先は`out/build/windows-x64/Release/`または`Debug/`です。

## プリセットを使わない場合

```powershell
cmake -B build -S .
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure --no-tests=error
```

EXEは`build/Release/MTQQToaster.exe`です。起動後の操作は[操作マニュアル](../user-guide.md)を参照してください。
Debugの場合は、ビルドとテストの`Release`を`Debug`に置き換えてください。
アプリと依存ライブラリはx64・静的CRT（Release: `/MT`、Debug: `/MTd`）でビルドします。
対応するWindows上では追加のWindows App Runtime・.NET・VC++ Redistributableのインストールは不要です。実行モジュールは`MTQQToaster.exe`だけです（設定例と依存ライブラリのライセンス文書は別途配布）。MQTTは静的リンクしたcoreMQTT v2.3.1、TLSはOS標準のSchannelを利用し、OpenSSLは不要です。Windows 10 version 1809以降／Windows 11を対象とします。

## 以前の構成で失敗していた場合

古いtoolchainやgeneratorの設定が`build`に残っている場合は、一度だけキャッシュを作り直します。

```powershell
cmake --fresh -B build -S .
```

`cmake --version`が4.2未満の場合はVS2026のDeveloper PowerShellで実行し直してください。
vcpkgが見つからない場合は、Visual Studio Installerで上記コンポーネントを確認します。
独自のvcpkgを使う場合は`VCPKG_ROOT`、または`-DCMAKE_TOOLCHAIN_FILE=.../scripts/buildsystems/vcpkg.cmake`で明示できます。変更時は新しいビルドディレクトリか`--fresh`を使ってください。
以前に設定した`VCPKG_ROOT`からVS付属vcpkgへ戻す場合は、そのシェルで`$env:VCPKG_ROOT = $null`を実行してから`cmake --fresh -B build -S .`を実行します。

参考: [VS2026のC++コンポーネント](https://learn.microsoft.com/en-us/visualstudio/install/workload-component-id-vs-community?view=vs-2026)、[vcpkgのCMake連携](https://learn.microsoft.com/en-us/vcpkg/users/buildsystems/cmake-integration)、[CMakeのVS2026対応](https://cmake.org/cmake/help/latest/generator/Visual%20Studio%2018%202026.html)。


## 生成物の整理

通常の生成先は`out/build/windows-x64`です。`build`はプリセットなしの手順で指定した場合だけ使います。検証用に別の生成先を作った場合は、検証後に片付けます。

生成物を編集・コミットしません。削除前に起動中のEXEと残したい実行時データがないことを確認します。削除後は構成・ビルドで再生成できます。

# MSVC / CMake / vcpkg のビルドとテスト

## リポジトリの構成

- `MTQQToasterLib`: `app_logic.cpp`を含む静的ライブラリ。
- `MTQQToaster`: `main.cpp`と`app.rc`を含むWin32 EXE。
- `MTQQToasterTests`: `tests/test_main.cpp`を使うGoogleTest EXE。`gtest_discover_tests`でCTestに登録する。
- C++23が必須で、MSVC以外は`CMakeLists.txt`で拒否する。
- vcpkgマニフェストはmosquitto、spdlog、nlohmann-json、gtestを指定し、`builtin-baseline`を固定している。更新は依頼に必要な範囲で行う。
- プリセットは`x64-debug`と`x64-release`。Visual Studio 18 2026 generatorで`out/build/windows-x64`を共有し、build/testプリセットで構成を選ぶ。
- プリセットなしでも`cmake -B build -S .`でVS付属vcpkgを自動検出する。`x64-windows-static`と静的CRT（Debug: /MTd、それ以外: /MT）は`project()`より前に設定する。

## 環境を確認する

Visual Studio 2026 Communityの「C++によるデスクトップ開発」を、MSVC x64/x86、Windows SDK、CMakeツール、vcpkgを含めてインストールする。必要なコンポーネントは`.vsconfig`にも記載している。
Developer PowerShell for VS 2026でリポジトリルートから実行する。VS generatorはコンパイラを検出するため、CMakeがPATHにあれば通常のPowerShellでもよい。

```powershell
Get-Command cmake, ctest -ErrorAction Stop
cmake --version
cmake --list-presets=all
```

CMake 4.2以降が必要（VS2026 generator対応）。VS付属版を使えば別途導入は不要。
`cmake/ConfigureVcpkg.cmake`は明示的なtoolchain、`VCPKG_ROOT`、VS付属vcpkgの順に選択する。VSはgeneratorの選択先、`VSINSTALLDIR`、`vswhere`から検出する。
通常は環境変数の設定、vcpkgのclone/bootstrap/integrateは不要。初回の依存復元にはネットワーク接続が必要。
ユーザー固有の絶対パスを共有プリセットへ書かない。Ninjaを明示する場合だけ、開発者シェルとNinjaが利用できることを確認する。

## Debugの標準手順

```powershell
cmake --preset x64-debug
cmake --build --preset x64-debug --parallel
ctest --preset x64-debug
```

各コマンドの終了コードを確認し、失敗した段階で原因を調べる。構成失敗後に古いテストEXEを実行して成功扱いにしない。
CTestでテストが0件の場合も検証完了にしない。

Releaseを確認する場合:

```powershell
cmake --preset x64-release
cmake --build --preset x64-release --parallel
ctest --preset x64-release
```

プリセットなしの標準手順も確認する:

```powershell
cmake -B build -S .
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure --no-tests=error
```

## 診断の順序

- 構成失敗: CMakeの版、VSのC++ワークロード、SDK、検出したvcpkgのパスと復元ログを確認する。明示した`VCPKG_ROOT`やtoolchainがあればそちらが優先される。
- キャッシュ不一致: `CMakeCache.txt`のgenerator、toolchain、triplet、ソースディレクトリを確認する。設定変更時は`cmake --fresh -B build -S .`または新しいビルドディレクトリを使う。生成物やキャッシュは直接編集しない。
- コンパイル失敗: 最初の診断と対象ターゲットを確認する。使用する標準ライブラリと`winrt/Windows.*.h`を明示的にincludeし、偶然の推移的includeに依存しない。
- リンク失敗: WinRT/Win32ライブラリ、vcpkgのimported target、x64、CRTの一致を確認する。無関係なライブラリ追加や警告抑制で回避しない。
- 日本語の文字化け: ソースのUTF-8と対象ターゲットの`/utf-8`を確認する。このオプションは`MTQQToasterLib`からEXEとテストにも伝播する。
- テスト失敗: 入力・期待値・実装を比較し、既存の不具合と今回の回帰を区別する。テスト入力のJSONが意図した正常・異常ケースになっていることも確認する。

## 検証範囲

設定・ペイロードのテストは実際のブローカーやトースト表示を必要としない。
一時ファイルはテスト専用のパスを使い、成功・失敗にかかわらず片付ける。
通知、トレイ、設定監視、再接続、終了は変更に応じてWindowsの対話セッションで確認し、実際に行った確認を報告する。
設定やMQTT認証情報、ビルド生成物をコミット対象に含めない。

## 公式資料

- [CMake Presets](https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html)

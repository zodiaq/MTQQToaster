# MSVC / CMake / vcpkg のビルドとテスト

## リポジトリの構成

- `MTQQToasterLib`: `app_logic.cpp`を含む静的ライブラリ。
- `MTQQToaster`: `main.cpp`と`app.rc`を含むWin32 EXE。
- `MTQQToasterTests`: `tests/test_main.cpp`を使うGoogleTest EXE。`gtest_discover_tests`でCTestに登録する。
- C++23が必須で、MSVC以外は`CMakeLists.txt`で拒否する。
- vcpkgマニフェストはmosquitto、spdlog、nlohmann-json、gtestを指定し、`builtin-baseline`を固定している。更新は依頼に必要な範囲で行う。
- プリセットは`x64-debug`と`x64-release`。どちらもNinjaと`x64-windows-static`を使い、CRTはそれぞれ`MultiThreadedDebug`（/MTd）、`MultiThreaded`（/MT）。
- 現在はconfigureプリセットのみ。`cmake --build --preset x64-debug`や`ctest --preset x64-debug`に対応するbuild/testプリセットはない。

## 環境を確認する

Visual Studioのx64開発者用PowerShell等、MSVCとWindows SDKが有効なシェルでリポジトリルートから実行する。
通常のPowerShellでは`cl.exe`や`VSINSTALLDIR`が設定されていない場合がある。

```powershell
Get-Command cmake, ctest, ninja, cl -ErrorAction Stop
$env:VSINSTALLDIR
$env:VCPKG_ROOT
if ([string]::IsNullOrWhiteSpace($env:VCPKG_ROOT)) {
    throw 'Set VCPKG_ROOT to an existing vcpkg directory in this shell.'
}
Test-Path -LiteralPath (Join-Path $env:VCPKG_ROOT 'scripts/buildsystems/vcpkg.cmake')
cmake --list-presets
```

`VCPKG_ROOT`が空なら実在するvcpkgルートをこのシェルに設定してから`Test-Path`を実行する。ユーザー固有の絶対パスを共有プリセットへ書かない。
プリセットは`VSINSTALLDIR`配下の`SegmentHeap.cmake`も参照するため、コンパイラだけでなくこのパスの存在も確認する。
CMakeは`CMakeLists.txt`の最小要件に加え、プリセットのschema version 3に対応する版が必要。

## Debugの標準手順

```powershell
cmake --preset x64-debug
cmake --build out/build/x64-debug --parallel
ctest --test-dir out/build/x64-debug --output-on-failure --no-tests=error
```

各コマンドの終了コードを確認し、失敗した段階で原因を調べる。構成失敗後に古いテストEXEを実行して成功扱いにしない。
CTestでテストが0件の場合も検証完了にしない。

Releaseを確認する場合:

```powershell
cmake --preset x64-release
cmake --build out/build/x64-release --parallel
ctest --test-dir out/build/x64-release --output-on-failure --no-tests=error
```

## 診断の順序

- 構成失敗: 開発者シェル、`VSINSTALLDIR`、`VCPKG_ROOT`、Ninja、SDK、vcpkgの復元ログを確認する。
- キャッシュ不一致: `CMakeCache.txt`のgenerator、toolchain、triplet、ソースディレクトリを確認する。ルートや`build/`にある過去のVisual Studio生成物と、プリセットの`out/build/`を混用しない。
- コンパイル失敗: 最初の診断と対象ターゲットを確認する。使用する標準ライブラリと`winrt/Windows.*.h`を明示的にincludeし、偶然の推移的includeに依存しない。
- リンク失敗: WinRT/Win32ライブラリ、vcpkgのimported target、x64、CRTの一致を確認する。無関係なライブラリ追加や警告抑制で回避しない。
- 日本語の文字化け: ソースのUTF-8と対象ターゲットの`/utf-8`を確認する。現在このオプションは`MTQQToasterLib`にPRIVATEで設定され、EXEとテストには伝播しない。
- テスト失敗: 入力・期待値・実装を比較する。調査時点の`ConfigTest.LoadValidConfig`のJSONにはカンマ欠損と余分な閉じ括弧がある。現状を再確認し、実装の回帰と区別する。この記述を失敗の恒久的な免除にしない。

## 検証範囲

設定・ペイロードのテストは実際のブローカーやトースト表示を必要としない。
一時ファイルはテスト専用のパスを使い、成功・失敗にかかわらず片付ける。
通知、トレイ、設定監視、再接続、終了は変更に応じてWindowsの対話セッションで確認し、実際に行った確認を報告する。
設定やMQTT認証情報、ビルド生成物をコミット対象に含めない。

## 公式資料

- [CMake Presets](https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html)

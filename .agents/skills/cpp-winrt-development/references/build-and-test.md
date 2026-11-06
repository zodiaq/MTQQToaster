# MSVC / CMake / vcpkg の確認手順

この文書はエージェント向けの確認・診断基準を扱う。環境準備とコマンドの正本は[ビルド手順](../../../../docs/development/build.md)と[テスト手順](../../../../docs/development/testing.md)に置く。

## 作業前の確認

- `CMakeLists.txt`、`CMakePresets.json`、`vcpkg.json`、`vcpkg-configuration.json`を読み、対象ターゲットと依存関係を確認する。
- 標準プリセットはVS2026 generatorと`out/build/windows-x64`を使用する。DebugとReleaseは構成・依存関係を共有する。
- `x64-windows-static`と静的CRTの整合を保つ。ユーザー固有のパスを共有プリセットへ書かない。
- コード変更後は該当構成をビルドし、必要なテストを実行する。リリースや依存関係に関わる変更はReleaseも確認する。
- 構成・ビルド失敗後に古いEXEを実行して成功扱いにしない。CTestのテスト0件も検証完了としない。
- プリセットなしの構成を変更した場合は、その手順も確認する。文書だけの変更ではアプリ起動や通信を行わない。

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

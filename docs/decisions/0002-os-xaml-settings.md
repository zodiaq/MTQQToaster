# 0002：OS内蔵XAMLによる設定画面

[設計判断一覧](README.md)

- 日付：2026-11-07
- 状態：採用（この作業ツリーで実装。配布環境の検証範囲は下記参照）

## 背景と判断

設定項目・配置・保存方式を概ね維持し、Windowsの設定に近い余白、カード、入力部品へ変更する。C++23、C++/WinRT、CMake、静的CRT、x64-windows-staticは維持する。

OS内蔵のWindows.UI.XamlをDesktopWindowXamlSourceでWin32ウィンドウへ埋め込む。標準部品をC++で生成し、C#、.NET、Windows App SDK、WinUI 3、カスタムWinRT型、IDL、XAMLビルド工程を追加しない。既存の設定ロード・保存コールバック、画像選択・コピー、MQTT処理を利用する。

## 代替案

- Win32の独自描画：配布構成を保てるが、入力部品やアクセシビリティの独自対応が増える。
- WinUI 3：新しい外観の標準部品を利用できるが、Windows App SDKのビルド・配布依存が増える。

## 影響と制約

- OS下限をWindows 10 1903に変更する。MSIXのMinVersionも18362へ変更する。
- OS内蔵XAMLは最新WinUIと同じ外観を保証しない。旧世代のAPIであり、最新機能への追従を目的に採用しない。
- Windows App SDKへの依存は増やさない。ただし静的CRTだけで、OS内蔵XAMLの実行時依存が全環境で満たされるとは断定しない。公式資料の非MSIX向けVisual C++ Runtime要件を配布時に考慮する。
- 初回設定表示までXAMLを初期化しない。初回以降はホストを非表示で保持するため、閉じた後もXAMLのメモリは残る。
- 最終islandを開閉のたびに破棄する試作では、同じSTA上の設定監視テストがタイムアウトした。ホストをアプリ終了まで保持する構成で回帰テストが成功した。OS内部の原因までは特定していない。
- 終了時は購読解除、内容・island・HWND解放、WindowsXamlManager.Closeとメッセージ処理、COM終了の順に扱う。

## 検証と未解決事項

[検証記録](../validation/2026-11-07-os-xaml.md)に実行環境と結果を記載する。開発ツールのないWindows、Windows 10 1903、署名済みMSIXでの導入、実IMEの変換候補操作、異なるDPIのモニター間移動、テーマ／ハイコントラストの切替は未確認。OS内蔵XAML Islandsには実行中のテーマ追従に制約があり、完全な追従は保証しない。

## 参照

- [標準部品のC++ホスティング](https://learn.microsoft.com/en-us/windows/uwp/xaml-islands/host-standard-control-with-xaml-islands-cpp)
- [要件と制約](https://learn.microsoft.com/en-us/windows/uwp/xaml-islands/xaml-islands)
- [キーボードとフォーカス](https://learn.microsoft.com/en-us/windows/uwp/xaml-islands/advanced-scenarios-xaml-islands-cpp)
- [WindowsXamlManager.Close](https://learn.microsoft.com/en-us/uwp/api/windows.ui.xaml.hosting.windowsxamlmanager.close)
- [旧XAML Islandsに関する告知](https://github.com/CommunityToolkit/Microsoft.Toolkit.Win32#xaml-islands-notice)
- 実装：[settings_ui.cpp](../../src/settings_ui.cpp)、[main.cpp](../../src/main.cpp)

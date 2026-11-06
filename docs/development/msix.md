# MSIXパッケージ

[ドキュメント一覧](../README.md) · [ビルド手順](build.md)

既存のWin32 EXEをWindows SDKのMakeAppxでパッケージ化します。アプリをUWPへ移行せず、設定画面にOS内蔵UWP XAMLを使用します。WinUI 3とWindows App SDKは不要です。Windows 10 version 1903以降／Windows 11のx64が対象です。

## 作成

ビルド環境にWindows SDKのMakeAppxとSignToolを含め、リポジトリのルートで実行します。

```powershell
./scripts/package-msix.ps1
```

Releaseの構成・ビルド・CTestが成功した場合だけ、`out/msix/MTQQToaster_1.0.0.0_x64.msix`を作ります。EXE、設定例、パッケージ用PNG、アプリと依存ライブラリのライセンスを同梱します。個人の設定・ログ・証明書は同梱しません。ステージングは実行ごとに別ディレクトリを作成します。

パッケージ画像は`resources/package-icon.png`（白い図柄と濃いグレーの不透明背景）から44・50・150pxで生成します。トレイのテーマ別ICOとは別に管理し、明るい背景でも図柄を識別できるようにしています。

配布先に合わせてIDと発行者を指定できます。

```powershell
./scripts/package-msix.ps1 -IdentityName 'YourCompany.MTQQToaster' `
    -Publisher 'CN=YourCompany' -PublisherDisplayName 'YourCompany' -Version '1.0.1.0'
```

デフォルトの発行者は開発用の仮値です。Publisherは署名証明書のSubjectと一致させます。更新時はNameとPublisherを維持し、4桁のVersionを増やしてください。Store配布ではPartner Centerで指定された値を使用します。

## 署名とインストール

開発用セットアップ一式を作成する場合は`./scripts/create-dev-setup.ps1`を実行します。ビルド・テスト後、CurrentUserのMyストアに開発用コード署名証明書を作成（有効な既存証明書は再利用）し、署名済みMSIX・公開証明書・導入手順を`out/setup/MTQQToaster-1.0.0.0-x64-setup.zip`にまとめます。秘密鍵はエクスポートせず、信頼ストアへの登録とインストールは行いません。CMS署名を検証しますが、信頼された発行者としての検証や実機導入確認とは別です。

生成物は未署名です。通常のインストールには、対象PCが信頼するコード署名証明書での署名が必要です。証明書の生成・信頼ストアへの登録・インストールはスクリプトでは行いません。

SDKのx64ディレクトリにあるSignToolで、CurrentUserのMyストアに用意した証明書を指定します。

```powershell
signtool sign /fd SHA256 /sha1 <証明書の拇印> out/msix/MTQQToaster_1.0.0.0_x64.msix
signtool verify /pa /v out/msix/MTQQToaster_1.0.0.0_x64.msix
Add-AppxPackage -Path out/msix/MTQQToaster_1.0.0.0_x64.msix
```

配布用署名では証明書提供者のタイムスタンプURLを`/tr`で指定し、`/td SHA256`を併用してください。開発用証明書を使う場合も秘密鍵をリポジトリへ保存しません。

## EXE版との動作差

- MSIX版の通知元はパッケージのアプリIDです。ペイロードのtitleは通知内容として扱います。通知クリックからのプロセス起動・COM activationは今回の対象外です。
- 自動起動は初期状態で無効です。トレイの「Windowsのスタートアップ設定...」からOSの設定を開き、MTQQToasterを有効にします。MSIX版はRunキーを書き換えません。
- パッケージのインストール先は読み取り専用です。設定例は初回取り込み用で、設定・ログは既存のAppData経路を使用します。MSIXのAppData仮想化が適用されるため、EXE版とのデータ共有を保証しません。アンインストールで仮想化されたデータが削除される場合があります。必要な設定は事前に退避してください。
- EXE版からの移行では、EXE版の自動起動を解除して終了してからMSIX版を起動してください。

## 実機確認

署名済みパッケージでインストール、スタートメニュー起動、設定保存と再起動、テキスト／画像通知、自動起動の有効・無効、バージョン更新、アンインストールを確認します。MakeAppx成功や単体テストだけでは、これらの動作確認を代替できません。

参考: [Microsoftの手動パッケージ化手順](https://learn.microsoft.com/en-us/windows/msix/desktop/desktop-to-uwp-manual-conversion)、[MSIXの実行時動作](https://learn.microsoft.com/en-us/windows/msix/desktop/desktop-to-uwp-behind-the-scenes)。

# C++/WinRT と Win32 の確認事項

## スレッドとapartment

- `WinMain`は`init_apartment(apartment_type::single_threaded)`でSTAを初期化し、Win32メッセージループを実行する。
- `MqttWorkerThread`は独立した`std::jthread`で、`Uri`や`ShowToast`を通じてWinRTを呼ぶ。初期化はスレッド単位なので、変更時はワーカー側も確認する。
- 自分で作るWinRT利用スレッドには用途に合うapartmentを明示し、初期化に成功した場合のみ同じスレッドで対応する終了処理を行う。モード不一致のHRESULTを握りつぶさない。
- STAやUI上で非同期操作の`.get()` / `.wait_for()`や長いブロッキング処理を行わない。Win32 UIへの操作は所有スレッドへメッセージ等で渡し、データの寿命を保証する。
- `resume_background`後に元のSTAが必要なら`apartment_context`等で復帰する。MTA内の継続が同じOSスレッドへ戻ると仮定しない。

## 非同期処理とイベントの寿命

- suspension後に使う文字列・パス・WinRTオブジェクトは原則として値でコルーチンに渡す。参照パラメータや一時ラムダのキャプチャを寿命保証に使わない。
- WinRT実装クラスのメンバーコルーチンでは必要に応じて最初のsuspension前に`get_strong()`を取得する。通常のC++クラスにはこのAPIがないので、そのクラスの所有方式に合わせる。
- `IAsyncAction` / `IAsyncOperation<T>`はawait、完了ハンドラー、管理用オブジェクト等で完了と例外を観測する。破棄だけでキャンセルや完了待ちが済むと考えない。
- `fire_and_forget`を使う場合は処理内で例外を扱い、停止と寿命を別途設計する。未処理例外はプロセス終了につながる。
- 監視元、event tokenまたはrevoker、初期化操作の所有者を明確にする。APIが対応する場合は`auto_revoke`を利用できるが、手動解除でもよい。
- 購読解除だけでは実行中のハンドラーの完了を保証しない。終了フラグ、初期化処理との同期、必要な完了待ちを組み合わせ、停止後の再登録を防ぐ。
- `g_config`、`g_lastWriteTime`、`g_queryResult`など、複数の呼び出しから使う状態の同期を確認する。イベントの直列実行を根拠なく仮定しない。
- 終了処理でSTAへの復帰が必要なら、メッセージループを動かしたまま完了を処理できる手順にする。ロックを保持したままjoinやawaitを行わない。

## 通知、文字列、パス

- 現在は`Windows.UI.Notifications.ToastNotificationManager`とXML DOMを使用する。WinUIやWindows App SDKへの移行は、その必要がある依頼で扱う。
- 現在のデスクトップ通知経路に必要なAppUserModelIDとショートカット等の登録条件を確認する。ペイロードの`title`を安定したアプリIDの代わりに使わない。
- タイトルと本文が選んだXMLテンプレートのどのノードへ入るかを確認する。文字列の連結でXMLを組み立てず、DOMのテキストノード・属性を使う。
- MQTT/JSONはUTF-8、Windows APIはUTF-16を基準に、`winrt::to_hstring` / `winrt::to_string`等で変換する。日本語を`char` / `wchar_t`の単純コピーで変換しない。
- 相対画像パスは設定フォルダー直下、`ToastAssets`、EXEのディレクトリの順に解決する。[ペイロード仕様](../../../../docs/reference/payload.md)を参照する。HTTP/HTTPS画像を拒否するため、対応範囲を勝手に拡張しない。
- ファイルパスとURIを区別し、空白、日本語、UNC、存在しないファイルを確認する。ネットワークパスや未対応スキームの扱いは要求に応じて明確にする。
- テキストだけ、画像付き、XML特殊文字、日本語を含む入力を変更に応じて実機確認する。`Show()`が例外なく戻ったことだけで画面表示を断定しない。

## Win32 / MQTT / 診断

- HANDLE、HKEY、HMENU、HICON、HWNDはそれぞれのAPIに合う解放方法と所有権で管理する。借用リソースを解放せず、早期returnでも所有リソースを失わない。
- `SRWLOCK`はRAII等で例外安全に解放する。設定コピーや解析で例外が出てもロックが残らないようにする。
- coreMQTTの通信コールバックやWndProcなどのC ABI境界では例外を処理し、WinRTのHRESULTとメッセージ、Win32の`GetLastError()`、coreMQTTの戻り値を混同しない。
- Win32のエラー値は失敗が判明した直後に取得する。ログには処理名とエラーコードを残し、認証情報や不用意な生ペイロード出力を避ける。
- MQTTペイロードは明示的な長さで扱い、必要なら所有するバッファへコピーしてから別スレッドへ渡す。
- coreMQTTセッションと通信層の生成、接続、ループ、切断、破棄を対応させ、停止要求で待機や再接続ループから抜けられるようにする。
- TLSはSchannelとWindowsの証明書検証を使用する。証明書チェーン・失効・接続先名の検証を維持する。詳細は[通信層](../../../../docs/mqtt-transport.md)を参照する。

## 公式資料

APIの利用条件やSDKバージョンに不明点があるとき、該当する資料を確認する。

- [Get started with C++/WinRT](https://learn.microsoft.com/en-us/windows/apps/develop/cpp-winrt/get-started)
- [Concurrency and asynchronous operations](https://learn.microsoft.com/en-us/windows/apps/develop/cpp-winrt/concurrency)
- [Advanced concurrency and asynchrony](https://learn.microsoft.com/en-us/windows/apps/develop/cpp-winrt/concurrency-2)
- [Strong and weak references](https://learn.microsoft.com/en-us/windows/apps/develop/cpp-winrt/weak-references)
- [Error handling](https://learn.microsoft.com/en-us/windows/apps/develop/cpp-winrt/error-handling)
- [Desktop toast notifications and AppUserModelID](https://learn.microsoft.com/en-us/windows/win32/shell/enable-desktop-toast-with-appusermodelid)

# MQTT / Schannel 通信層

更新日: 2026-11-07。coreMQTT v2.3.1への移行を実装。mosquitto、OpenSSL、pthreadsは依存から除去した。

## 構成

- `src/mqtt_client.cpp`: URL・topic検証、coreMQTTの接続／購読／受信ループ、SUBACK確認、topic照合。
- `src/mqtt_transport.cpp`: 非同期DNS、非ブロッキングWinsock、Schannel、Windows証明書検証。C ABIの送受信コールバックから例外を漏らさない。
- `src/main.cpp`: MTA初期化済みワーカーで設定スナップショットを取得し、接続単位で通信層を所有する。受信topicに対応する画像を付けて既存の通知処理を呼ぶ。
- `ports/coremqtt`: vcpkg overlay port。v2.3.1とSHA-512を固定し、静的CRTでビルドする。ライセンスはMIT。FreeRTOS OS自体は導入しない。

coreMQTTのmainはMQTT 5系であるため、MQTT 3.1.1対応版のv2.3.1を明示している。[公式ソース](https://github.com/FreeRTOS/coreMQTT/tree/v2.3.1)、[transport interface](https://github.com/FreeRTOS/coreMQTT/blob/v2.3.1/source/interface/transport_interface.h)

## 接続・停止

`mqtt://host[:port]`と`mqtts://host[:port]`を受け付ける。省略時のポートは1883／8883。IPv6は角括弧で囲み、IDNホスト名はWindows APIでASCIIへ変換する。URLに認証情報やパスは含めない。

clean sessionで接続し、topicごとにQoS 0で購読してSUBACKを確認する。拒否・不正応答・読み書き失敗では接続を破棄する。keep-aliveは60秒、PINGRESP待ちは5秒。切断後は5秒のバックオフを経て新しいセッションを作り、全topicを再購読する。

設定変更・終了はDNS、TCP接続、TLS、MQTTループ、バックオフ中にも観測する。ソケット待ちは最大100msずつ。DNSとTCP接続は各5秒、TLSハンドシェイクは10秒、送信とCONNACK／SUBACK待ちは5秒を上限とする。Windows証明書チェーン取得には累積2秒のURL取得タイムアウトを指定するため、検証中の停止にはその待ち時間が加わりうる。DNSのキャンセル後は非同期完了を待ってからOVERLAPPEDを解放する。

キャンセル時はDISCONNECT送信のために停止を遅らせず、ソケットを閉じる。Willは使用せず、次回接続はclean sessionとする。MQTTワーカーの再接続フラグは設定コピーの前に消費し、コピー後に到着した更新を失わない。

## TLSと証明書

対象OSはWindows 10 version 1809以降／Windows 11。`SCH_CREDENTIALS`でTLS 1.2以降を許可し、古いSSL／TLSを無効にする。暗号処理はSchannel、証明書検証はWindows CryptoAPIを利用する。[SCH_CREDENTIALS](https://learn.microsoft.com/en-us/windows/win32/api/schannel/ns-schannel-sch_credentials)

ハンドシェイク後、Windowsの信頼ストアでチェーン・サーバー用途・有効期限・失効状態を検証し、SSLポリシーで接続先名を確認する。検証完了前にはMQTTデータや認証情報を送れない。証明書検証を無効にするオプションは設けない。Schannel内部の自動検証に代えて明示検証する構成であり、検証省略ではない。

以前の接続先名検証の無効化は廃止した。独自CAの場合はWindows側でCAを信頼させ、証明書のSANに一致する名前で接続する。失効確認不能を含む証明書エラーは接続失敗として扱う。クライアント証明書認証は実装しない。

TLSの部分受信、複数レコード、余剰暗号データ、復号済みデータの残り、部分送信を保持する。再ネゴシエーション／TLS 1.3のハンドシェイク継続では再度Schannelを呼び、証明書を再検証する。

## 入力上限

- topic: 1～128件。各フィルターはMQTTのUTF-8文字列上限65535 bytes以内。`+`／`#`はレベル境界の規則に従う。
- 受信MQTTパケット全体: 256 KiB。超過した場合は切断・再接続する。
- TLS入力バッファ: 256 KiB。過大なハンドシェイクや不正データを無制限に蓄積しない。
- ユーザー名／パスワード: MQTTの各長さ上限65535 bytes。パスワード指定時はユーザー名が必要。
- MQTT 5、WebSocket、永続セッション、クライアント証明書認証は対象外。

## 検証

2026-11-07のMSVC x64 Releaseビルドでは、EXEは912,896 bytes。Win32設定画面＋mosquitto/OpenSSL構成の6,093,312 bytesから約85%削減した。`dumpbin /dependents`で依存DLLがWindows標準のものだけであることを確認した。Windows App Runtime、OpenSSL DLL、VC++再頒布可能パッケージの追加インストールは不要。サイズはコンパイラーやビルド設定により変わる。

Debug／Releaseとも通常テスト25件とローカル通信テスト8件が成功した（CTest上は26項目）。設定画面の編集・保存・キャンセル、ウィンドウ位置復元、設定監視のスモークテストも両構成で成功した。

通常のGoogleTestでURL、UTF-8、ワイルドカード、`$SYS`、長さ上限、分割送受信、認証・購読拒否、SUBACKタイムアウト、過大パケット、keep-alive、停止、コールバック例外を検証する。

`MTQQTOASTER_TRANSPORT_TESTS=ON`で追加するローカル結合テストはPowerShell 7.5以降と.NET SslStreamをテストサーバーに使用する。TCP／TLSでバイナリデータを往復し、正しい証明書の接続成功、未知CA・接続先名不一致・期限切れそれぞれのエラーコード、TLS初期化中の停止、coreMQTTでの日本語topic受信と再接続・再購読を確認する。

テストクライアントはメモリ内だけの専用チェーンエンジンを使用する。OSの信頼ストアは変更しない。サーバー側はSchannelの制約上、一時鍵コンテナーを使用し、証明書破棄時に削除する。テストサーバーはループバックのみで待ち受ける。

実際のユーザー環境のブローカー、独自CAの運用、プロキシ／VPN、TLS再ネゴシエーションを強制するサーバーとの相互運用は別途確認が必要。テスト用サーバーはMQTTの必要な応答のみを実装するfixtureであり、フル機能のブローカーではない。

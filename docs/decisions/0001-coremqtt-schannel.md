# 0001：coreMQTTとSchannelの採用

[判断記録一覧](README.md)

- 状態：採用済み（実装済み）
- 記録日：2026-11-07
- 出典：従来の通信層資料にある移行記録。

## 背景と判断

MQTTクライアントをmosquittoからcoreMQTT v2.3.1へ移行し、通信をWinsock、TLSをWindows標準のSchannelで実装しました。mosquitto、OpenSSL、pthreadsを依存から除き、配布物を小さくする構成です。

MQTT 3.1.1対応のv2.3.1をローカルvcpkg overlay portで固定しています。FreeRTOS OS自体は導入しません。移行前後のサイズと試験結果は[検証記録](../validation/2026-11-07-mqtt-transport.md)を参照してください。

## 影響

DNS、TCP、TLS、証明書検証、部分送受信、停止をアプリの通信層が担います。以前の接続先名検証の無効化を廃止し、Windowsの信頼ストアと接続先名による検証を行います。

現行動作は[通信層](../mqtt-transport.md)と[仕様](../../SPECIFICATION.md)を参照してください。元資料にない代替案の比較評価は補っていません。

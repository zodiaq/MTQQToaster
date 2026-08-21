#include <gtest/gtest.h>
#include <fstream>
#include "../app_logic.h"

// テスト用のダミーグローバルフラグ
std::atomic<bool> g_running{ true };

// 2. 完全な JSON ペイロードのパース
TEST(JsonParserTest, ParseFullJson) {
    std::string jsonStr = R"({
        "title": "洗濯完了",
        "message": "洗濯機が終了しました",
        "image": "laundry.jpg"
    })";

    auto res = ParseNotificationPayload(jsonStr);

    EXPECT_EQ(res.title, "洗濯完了");
    EXPECT_EQ(res.message, "洗濯機が終了しました");
    EXPECT_EQ(res.imageUrl, "laundry.jpg");
}

// 3. 一部キー欠損時のフォールバック
TEST(JsonParserTest, ParsePartialJson) {
    std::string jsonStr = R"({ "message": "ドアが開きました" })";

    auto res = ParseNotificationPayload(jsonStr);

    EXPECT_EQ(res.title, MQTTTOASTER_DEFAULT_TITLE); // デフォルト値
    EXPECT_EQ(res.message, "ドアが開きました");
    EXPECT_EQ(res.imageUrl, "");
}

// 4. 生テキスト（非JSON）のパース
TEST(JsonParserTest, ParsePlainText) {
    std::string rawText = "Simple plain text message";

    auto res = ParseNotificationPayload(rawText);

    EXPECT_EQ(res.title, MQTTTOASTER_DEFAULT_TITLE);
    EXPECT_EQ(res.message, "Simple plain text message");
}

// 5. 設定ファイル読み込みテスト
TEST(ConfigTest, LoadValidConfig) {
    std::wstring configPath = L"test_config.json";

    // 一時テストファイル作成
    std::ofstream out(configPath);
    out << R"({
        "mqtt_url": "mqtt://10.0.0.1:1883",
        "mqtt_topics": [ "mqtt_toaster/#" ],
        "mqtt_user": "admin",
        "mqtt_pass": "password"
        "log_level": "DEBUG",
        "log_max_size_mb": 5,
        "log_max_files": 3
    }    })";
    out.close();

    AppConfig cfg;
    bool success = LoadConfig(configPath, cfg);

    EXPECT_TRUE(success);
    EXPECT_EQ(cfg.url, "mqtt://10.0.0.1:1883");
	EXPECT_EQ(cfg.topics.size(), 1);
	EXPECT_EQ(cfg.topics[0], "mqtt_toaster/#");
	EXPECT_EQ(cfg.user, "admin");
	EXPECT_EQ(cfg.pass, "password");
    EXPECT_EQ(cfg.logLevel, "DEBUG");
    EXPECT_EQ(cfg.maxLogSizeMb, 5);
	EXPECT_EQ(cfg.maxLogFiles, 3);

    // テスト後削除
    DeleteFileW(configPath.c_str());
}
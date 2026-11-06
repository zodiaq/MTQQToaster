#include <gtest/gtest.h>
#include <fstream>
#include <limits>
#include "app_logic.h"
#include "../resources/resource.h"

// テスト用のダミーグローバルフラグ
std::atomic<bool> g_running{ true };

TEST(TrayIconTest, ThemeResourcesLoadAtTraySizes) {
    // Load the built application's resources without starting MQTT or registering a tray icon.
    const auto module = LoadLibraryExW((GetModuleDirectory() / L"MTQQToaster.exe").c_str(),
        nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    ASSERT_NE(module, nullptr);
    for (const int resource : {IDI_TRAYICON_WHITE, IDI_TRAYICON_BLACK}) {
        for (const int size : {16, 20, 24, 32, 48}) {
            const auto icon = static_cast<HICON>(LoadImageW(module, MAKEINTRESOURCEW(resource),
                IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
            EXPECT_NE(icon, nullptr) << "resource=" << resource << ", size=" << size;
            if (icon) DestroyIcon(icon);
        }
    }
    FreeLibrary(module);
}

TEST(SettingsInputTest, AcceptsPositiveIntegersWithoutRounding) {
    size_t value = 0;
    ASSERT_TRUE(ParsePositiveSetting(L"0005", value));
    EXPECT_EQ(value, 5u);
    const auto maximum = (std::numeric_limits<size_t>::max)();
    ASSERT_TRUE(ParsePositiveSetting(std::to_wstring(maximum), value));
    EXPECT_EQ(value, maximum);
}

TEST(SettingsInputTest, RejectsInvalidNumbersWithoutChangingOutput) {
    for (const auto* input : {L"", L"0", L"-1", L"+1", L"1.5", L" 1", L"1 ", L"1e3", L"１２", L"nan"}) {
        size_t value = 7;
        EXPECT_FALSE(ParsePositiveSetting(input, value));
        EXPECT_EQ(value, 7u);
    }
    size_t value = 7;
    EXPECT_FALSE(ParsePositiveSetting(std::to_wstring((std::numeric_limits<size_t>::max)()) + L"0", value));
    EXPECT_EQ(value, 7u);
}

class WindowPlacementTest : public ::testing::Test {
protected:
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        (L"MTQQToaster-placement-" + std::to_wstring(GetCurrentProcessId()) + L".json");
    void TearDown() override {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
};

TEST_F(WindowPlacementTest, RoundTripNegativeMonitorCoordinates) {
    const SettingsWindowPlacement expected{-1700, 120, 760, 900, true};
    ASSERT_TRUE(SaveSettingsWindowPlacement(path, expected));
    ASSERT_EQ(LoadSettingsWindowPlacement(path), expected);
}

TEST_F(WindowPlacementTest, RejectsMissingAndMalformedGeometry) {
    EXPECT_FALSE(LoadSettingsWindowPlacement(path));
    for (const char* input : {
        "not json", R"({"version":1})",
        R"({"version":1,"x":0,"y":0,"width":-1,"height":900})",
        R"({"version":1,"x":0,"y":0,"width":660.5,"height":900})",
        R"({"version":1,"x":18446744073709551615,"y":0,"width":660,"height":900})",
        R"({"version":2,"x":0,"y":0,"width":660,"height":900})"}) {
        { std::ofstream output(path); output << input; }
        EXPECT_FALSE(LoadSettingsWindowPlacement(path)) << input;
    }
}

TEST_F(WindowPlacementTest, RecoversOffscreenAndOversizedWindow) {
    const RECT workArea{-1920, 40, 0, 1080};
    EXPECT_EQ(FitSettingsWindowToWorkArea({4000, 4000, 660, 800}, workArea),
              (SettingsWindowPlacement{-660, 280, 660, 800}));
    EXPECT_EQ(FitSettingsWindowToWorkArea({-4000, -4000, 5000, 4000}, workArea),
              (SettingsWindowPlacement{-1920, 40, 1920, 1040}));
    EXPECT_EQ(FitSettingsWindowToWorkArea({-1800, 80, 660, 800}, workArea),
              (SettingsWindowPlacement{-1800, 80, 660, 800}));
}

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
        "mqtt_pass": "password",
        "log_level": "DEBUG",
        "log_max_size_mb": 5,
        "log_max_files": 3
    })";
    out.close();

    AppConfig cfg;
    bool success = LoadConfig(configPath, cfg);

    EXPECT_TRUE(success);
    EXPECT_EQ(cfg.url, "mqtt://10.0.0.1:1883");
	EXPECT_EQ(cfg.topics.size(), 1);
	EXPECT_EQ(cfg.topics[0].topic, "mqtt_toaster/#");
	EXPECT_TRUE(cfg.topics[0].image.empty());
	EXPECT_EQ(cfg.user, "admin");
	EXPECT_EQ(cfg.pass, "password");
    EXPECT_EQ(cfg.logLevel, "DEBUG");
    EXPECT_EQ(cfg.maxLogSizeMb, 5);
	EXPECT_EQ(cfg.maxLogFiles, 3);

    // テスト後削除
    DeleteFileW(configPath.c_str());
}

TEST(ConfigTest, SaveAndLoadConfig) {
    const auto configPath = std::filesystem::temp_directory_path() / L"mtqqtoaster_save_test.json";
    AppConfig expected;
    expected.url = "mqtts://broker.example.com:8883";
    expected.topics = { {"home/notification", "topic-image-test.png"}, {"alerts/#", ""} };
    expected.user = "test-user";
    expected.pass = "test-pass";
    expected.logLevel = "WARN";
    expected.maxLogSizeMb = 8;
    expected.maxLogFiles = 4;

    ASSERT_TRUE(SaveConfig(configPath, expected));

    AppConfig actual;
    ASSERT_TRUE(LoadConfig(configPath, actual));
    EXPECT_EQ(actual.url, expected.url);
    ASSERT_EQ(actual.topics.size(), expected.topics.size());
    EXPECT_EQ(actual.topics[0].topic, expected.topics[0].topic);
    EXPECT_EQ(actual.topics[0].image, expected.topics[0].image);
    EXPECT_EQ(actual.topics[1].topic, expected.topics[1].topic);
    EXPECT_EQ(actual.topics[1].image, expected.topics[1].image);
    EXPECT_EQ(actual.user, expected.user);
    EXPECT_EQ(actual.pass, expected.pass);
    EXPECT_EQ(actual.logLevel, expected.logLevel);
    EXPECT_EQ(actual.maxLogSizeMb, expected.maxLogSizeMb);
    EXPECT_EQ(actual.maxLogFiles, expected.maxLogFiles);

    std::error_code ec;
    std::filesystem::remove(configPath, ec);
}

TEST(ConfigTest, CopiesImageAndRemovesUnusedManagedImages) {
    const auto testDirectory = std::filesystem::temp_directory_path() / L"mtqqtoaster_image_test";
    std::error_code ec;
    std::filesystem::remove_all(testDirectory, ec);
    std::filesystem::create_directories(testDirectory);
    const auto source = testDirectory / L"source.png";
    {
        std::ofstream output(source, std::ios::binary);
        output << "test-image";
    }

    std::string copiedFile;
    ASSERT_TRUE(CopyToastImage(testDirectory, source, copiedFile));
    EXPECT_TRUE(std::filesystem::exists(testDirectory / copiedFile));

    const auto unused = testDirectory / L"topic-image-unused.png";
    {
        std::ofstream output(unused, std::ios::binary);
        output << "unused";
    }
    RemoveUnusedToastImages(testDirectory, {{"test/#", copiedFile}});
    EXPECT_TRUE(std::filesystem::exists(testDirectory / copiedFile));
    EXPECT_FALSE(std::filesystem::exists(unused));
    EXPECT_TRUE(std::filesystem::exists(source));

    std::filesystem::remove_all(testDirectory, ec);
}

TEST(ConfigTest, SaveRejectsIncompleteConfig) {
    AppConfig config;
    config.url.clear();
    EXPECT_FALSE(SaveConfig(std::filesystem::temp_directory_path() / L"mtqqtoaster_invalid_test.json", config));
}

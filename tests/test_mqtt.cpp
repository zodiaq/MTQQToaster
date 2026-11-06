#include "mqtt_client.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <thread>

TEST(MqttEndpointTest, ParsesDefaultsPortsAndIpv6) {
    auto endpoint = ParseMqttEndpoint("mqtt://localhost");
    EXPECT_EQ(endpoint.host, "localhost"); EXPECT_EQ(endpoint.port, 1883); EXPECT_FALSE(endpoint.tls);
    endpoint = ParseMqttEndpoint("mqtts://[::1]:9999");
    EXPECT_EQ(endpoint.host, "::1"); EXPECT_EQ(endpoint.port, 9999); EXPECT_TRUE(endpoint.tls);
    EXPECT_EQ(ParseMqttEndpoint("mqtts://broker.example").port, 8883);
    EXPECT_TRUE(ParseMqttEndpoint("MQTTS://broker.example").tls);
}

TEST(MqttEndpointTest, RejectsAmbiguousOrCredentialBearingUrls) {
    for (const auto* value : {"http://localhost", "mqtt://", "mqtt://a:0", "mqtt://a:65536", "mqtt://a:",
        "mqtt://a:abc", "mqtt://user:pass@host", "mqtt://a/path", "mqtt://a?x=y", "mqtt://a#fragment",
        "mqtt://::1", "mqtt://[::1", "mqtt://[::1]garbage", "mqtt://[host]", "mqtt://[::zz]", "mqtt:// a"}) {
        EXPECT_THROW(ParseMqttEndpoint(value), std::invalid_argument) << value;
    }
    EXPECT_THROW(ParseMqttEndpoint(std::string("mqtt://a\0b", 10)), std::invalid_argument);
}

TEST(MqttFilterTest, ValidatesWildcardsUtf8AndLengths) {
    for (const auto* filter : {"#", "+", "+/+/value", "a/#", "a/+/c", "/", "日本語/通知", "$SYS/#"}) EXPECT_TRUE(IsValidMqttFilter(filter));
    for (const auto* filter : {"", "a#", "a/+b", "a/#/b", "a/b+", "a/##", "\xc0\xaf"}) EXPECT_FALSE(IsValidMqttFilter(filter));
    EXPECT_FALSE(IsValidMqttFilter(std::string("a\0b", 3)));
    EXPECT_FALSE(IsValidMqttFilter(std::string(65536, 'a')));
    EXPECT_TRUE(IsValidMqttFilter(std::string(65535, 'a')));
}

TEST(MqttFilterTest, MatchesLevelsIncludingSystemTopics) {
    EXPECT_TRUE(MqttTopicMatches("a/#", "a"));
    EXPECT_TRUE(MqttTopicMatches("a/+/c", "a//c"));
    EXPECT_FALSE(MqttTopicMatches("a/+", "a"));
    EXPECT_FALSE(MqttTopicMatches("a/+", "a/b/c"));
    EXPECT_FALSE(MqttTopicMatches("#", "$SYS/status"));
    EXPECT_FALSE(MqttTopicMatches("+/status", "$SYS/status"));
    EXPECT_TRUE(MqttTopicMatches("$SYS/#", "$SYS/status"));
    EXPECT_TRUE(MqttTopicMatches("日本語/+", "日本語/通知"));
}

TEST(MqttConfigTest, RejectsInvalidSubscriptionsAndCredentials) {
    AppConfig config;
    config.pass = "secret";
    EXPECT_THROW(ValidateMqttConfig(config), std::invalid_argument);
    config.user = "user";
    EXPECT_NO_THROW(ValidateMqttConfig(config));
    config.pass.clear();
    EXPECT_NO_THROW(ValidateMqttConfig(config));
    config.topics.clear();
    EXPECT_THROW(ValidateMqttConfig(config), std::invalid_argument);
    config.topics.assign(129, {"a", ""});
    EXPECT_THROW(ValidateMqttConfig(config), std::invalid_argument);
}

namespace {

// A deterministic broker double at the wire boundary. Only the production library parses incoming MQTT.
class BrokerDouble : public MqttTransport {
public:
    std::deque<uint8_t> incoming;
    std::vector<uint8_t> outgoing;
    std::vector<std::vector<uint8_t>> packets;
    int subscriptions = 0;
    bool rejectConnect = false;
    bool rejectSubscribe = false;
    bool wrongIdentifier = false;
    bool quiet = false;
    bool publish = false;
    bool oversized = false;
    bool ping = false;
    bool omitSuback = false;
    bool omitPingResponse = false;
    int pingCount = 0;
    std::vector<uint8_t> payload{'a', 0, 'b'};

    void Queue(std::initializer_list<uint8_t> bytes) { incoming.insert(incoming.end(), bytes); }
    int32_t Send(const void* data, size_t length) noexcept override {
        try {
            const auto count = (std::min)(length, size_t{2});
            const auto* begin = static_cast<const uint8_t*>(data);
            outgoing.insert(outgoing.end(), begin, begin + count);
            if (outgoing.size() >= 2) {
                size_t remaining = 0, multiplier = 1, offset = 1;
                while (offset < outgoing.size()) {
                    const auto byte = outgoing[offset++];
                    remaining += (byte & 127) * multiplier;
                    multiplier *= 128;
                    if (!(byte & 128)) {
                        if (outgoing.size() >= offset + remaining) {
                            packets.emplace_back(outgoing.begin(), outgoing.begin() + offset + remaining);
                            const auto type = outgoing[0] & 0xf0;
                            if (!quiet && type == 0x10) Queue({0x20, 2, 0, static_cast<uint8_t>(rejectConnect ? 5 : 0)});
                            if (!quiet && type == 0x80) {
                                ++subscriptions;
                                if (!omitSuback) Queue({0x90, 3, outgoing[offset], static_cast<uint8_t>(outgoing[offset + 1] + (wrongIdentifier ? 1 : 0)),
                                    static_cast<uint8_t>(rejectSubscribe ? 0x80 : 0)});
                                if (publish && subscriptions == 2) {
                                    Queue({0x30, static_cast<uint8_t>(5 + payload.size()), 0, 3, 'a', '/', 'b'});
                                    incoming.insert(incoming.end(), payload.begin(), payload.end());
                                }
                                if (oversized) Queue({0x30, 0x80, 0x80, 0x20}); // 524288-byte remaining length.
                            }
                            if (type == 0xc0) { ping = true; ++pingCount; if (!omitPingResponse) Queue({0xd0, 0}); }
                            outgoing.erase(outgoing.begin(), outgoing.begin() + offset + remaining);
                        }
                        break;
                    }
                }
            }
            return static_cast<int32_t>(count);
        }
        catch (...) { return -1; }
    }
    int32_t Receive(void* data, size_t size) noexcept override {
        if (incoming.empty()) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); return 0; }
        if (!size) return 0;
        *static_cast<uint8_t*>(data) = incoming.front(); incoming.pop_front(); return 1;
    }
};

MqttSessionOptions Options() { return {60, 500, "test-client"}; }

}

TEST(MqttSessionTest, HandlesPartialIoMultipleSubscriptionsAndBinaryPayload) {
    BrokerDouble broker;
    broker.publish = true;
    AppConfig config;
    config.user = "user";
    config.pass = "";
    config.topics = {{"a/+", ""}, {"other/#", ""}};
    bool received = false;
    RunMqttSession(config, broker, [&] { return received; }, [&](auto topic, auto payload) {
        EXPECT_EQ(topic, "a/b"); EXPECT_EQ(payload, std::string_view("a\0b", 3)); received = true;
    }, Options());
    EXPECT_TRUE(received);
    EXPECT_EQ(broker.subscriptions, 2);
    ASSERT_FALSE(broker.packets.empty());
    EXPECT_EQ(broker.packets[0][9] & 0xc2, 0xc2); // Username, empty password, clean session.
}

TEST(MqttSessionTest, RejectsAuthenticationAndSubscriptionFailures) {
    AppConfig config;
    for (int mode = 0; mode < 3; ++mode) {
        BrokerDouble broker;
        broker.rejectConnect = mode == 0;
        broker.rejectSubscribe = mode == 1;
        broker.wrongIdentifier = mode == 2;
        EXPECT_THROW(RunMqttSession(config, broker, [] { return false; }, [](auto, auto) {}, Options()), std::runtime_error);
    }
}

TEST(MqttSessionTest, RejectsOversizedPackets) {
    BrokerDouble broker;
    broker.oversized = true;
    EXPECT_THROW(RunMqttSession(AppConfig{}, broker, [] { return false; }, [](auto, auto) {}, Options()), std::runtime_error);
}

TEST(MqttSessionTest, CancelsAStalledConnackPromptly) {
    BrokerDouble broker;
    broker.quiet = true;
    const auto start = GetTickCount64();
    EXPECT_THROW(RunMqttSession(AppConfig{}, broker, [&] { return GetTickCount64() - start > 20; },
        [](auto, auto) {}, {60, 5000, "test-client"}), std::runtime_error);
    EXPECT_LT(GetTickCount64() - start, 500ULL);
}

TEST(MqttSessionTest, SendsKeepaliveOnAnIdleSubscription) {
    BrokerDouble broker;
    const auto start = GetTickCount64();
    RunMqttSession(AppConfig{}, broker, [&] { return (broker.pingCount >= 2 && broker.incoming.empty()) || GetTickCount64() - start > 4000; },
        [](auto, auto) {}, {1, 100, "test-client"});
    EXPECT_GE(broker.pingCount, 2);
    EXPECT_TRUE(broker.incoming.empty());
}

TEST(MqttSessionTest, TimesOutMissingSuback) {
    BrokerDouble broker;
    broker.omitSuback = true;
    EXPECT_THROW(RunMqttSession(AppConfig{}, broker, [] { return false; }, [](auto, auto) {},
        {60, 30, "test-client"}), std::runtime_error);
}

TEST(MqttSessionTest, DetectsAnUnresponsiveBrokerAfterPing) {
    BrokerDouble broker;
    broker.omitPingResponse = true;
    const auto start = GetTickCount64();
    EXPECT_THROW(RunMqttSession(AppConfig{}, broker, [&] { return GetTickCount64() - start > 8000; },
        [](auto, auto) {}, {1, 100, "test-client"}), std::runtime_error);
    EXPECT_GE(broker.pingCount, 1);
}

TEST(MqttSessionTest, ContainsApplicationExceptionsAtTheCallbackBoundary) {
    BrokerDouble broker;
    broker.publish = true;
    AppConfig config;
    config.topics = {{"a/+", ""}, {"other/#", ""}};
    EXPECT_THROW(RunMqttSession(config, broker, [] { return false; },
        [](auto, auto) { throw std::logic_error("notification failed"); }, Options()), std::logic_error);
}

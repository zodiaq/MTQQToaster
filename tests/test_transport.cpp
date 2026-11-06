#include "mqtt_client.h"
#include <wincrypt.h>
#include <gtest/gtest.h>
#include <atomic>
#include <fstream>
#include <thread>

struct WindowsMqttTransportTestAccess {
    static std::unique_ptr<WindowsMqttTransport> Create(MqttCancelled cancelled, HCERTCHAINENGINE engine) {
        return std::unique_ptr<WindowsMqttTransport>(new WindowsMqttTransport(std::move(cancelled), engine));
    }
};

namespace {

std::wstring Environment(const wchar_t* key)
{
    const auto length = GetEnvironmentVariableW(key, nullptr, 0);
    if (!length) throw std::runtime_error("Run transport tests through run_transport_tests.ps1");
    std::wstring value(length, L'\0');
    value.resize(GetEnvironmentVariableW(key, value.data(), length));
    return value;
}

uint16_t Port(const wchar_t* key) { return static_cast<uint16_t>(std::stoul(Environment(key))); }

class TestTrust {
public:
    HCERTSTORE store = nullptr;
    HCERTCHAINENGINE engine = nullptr;
    explicit TestTrust(const wchar_t* variable) {
        std::ifstream input(std::filesystem::path(Environment(variable)), std::ios::binary);
        const std::vector<char> bytes{std::istreambuf_iterator<char>(input), {}};
        store = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, nullptr);
        if (!store) throw std::runtime_error("Cannot open memory certificate store");
        if (!CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING, reinterpret_cast<const BYTE*>(bytes.data()),
            static_cast<DWORD>(bytes.size()), CERT_STORE_ADD_ALWAYS, nullptr)) {
            CertCloseStore(store, 0); throw std::runtime_error("Cannot add test root certificate");
        }
        CERT_CHAIN_ENGINE_CONFIG config{sizeof(config)};
        config.hExclusiveRoot = store;
        if (!CertCreateCertificateChainEngine(&config, &engine)) {
            CertCloseStore(store, 0); throw std::runtime_error("Cannot create exclusive test chain engine");
        }
    }
    ~TestTrust() { CertFreeCertificateChainEngine(engine); CertCloseStore(store, 0); }
};

void Echo(WindowsMqttTransport& transport)
{
    std::vector<uint8_t> expected(32769);
    for (size_t i = 0; i < expected.size(); ++i) expected[i] = static_cast<uint8_t>(i);
    size_t sent = 0;
    while (sent < expected.size()) {
        const auto count = transport.Send(expected.data() + sent, expected.size() - sent);
        ASSERT_GT(count, 0) << transport.LastError();
        sent += static_cast<size_t>(count);
    }
    std::vector<uint8_t> actual(expected.size());
    size_t received = 0;
    const auto deadline = GetTickCount64() + 10000;
    while (received < actual.size() && GetTickCount64() < deadline) {
        const auto count = transport.Receive(actual.data() + received, (std::min)(size_t{31}, actual.size() - received));
        ASSERT_GE(count, 0) << transport.LastError();
        received += static_cast<size_t>(count);
    }
    ASSERT_EQ(received, expected.size());
    EXPECT_EQ(actual, expected);
}

void ExpectCertificateFailure(WindowsMqttTransport& transport, const MqttEndpoint& endpoint, HRESULT expected)
{
    try { transport.Connect(endpoint); FAIL() << "Unacceptable certificate was accepted"; }
    catch (const std::exception& error) {
        EXPECT_EQ(std::string(error.what()), "TLS certificate validation failed (code " +
            std::to_string(static_cast<unsigned long>(expected)) + ")");
        EXPECT_EQ(transport.Send("credentials", 11), -1);
    }
}

}

TEST(WindowsTransportTest, PlainTcpPreservesPartialReadsAndBinaryData) {
    WindowsMqttTransport transport([] { return false; });
    transport.Connect({"127.0.0.1", Port(L"MTQQ_TEST_TCP_PORT"), false});
    Echo(transport);
}

TEST(WindowsTransportTest, SchannelValidatesTrustedCertificateAndPreservesRecords) {
    TestTrust trust(L"MTQQ_TEST_VALID_CERT");
    auto transport = WindowsMqttTransportTestAccess::Create([] { return false; }, trust.engine);
    transport->Connect({"localhost", Port(L"MTQQ_TEST_TLS_PORT"), true});
    Echo(*transport);
}

TEST(WindowsTransportTest, SchannelRejectsUnknownCa) {
    WindowsMqttTransport transport([] { return false; });
    ExpectCertificateFailure(transport, {"localhost", Port(L"MTQQ_TEST_TLS_PORT"), true}, CERT_E_UNTRUSTEDROOT);
}

TEST(WindowsTransportTest, SchannelRejectsHostNameMismatch) {
    TestTrust trust(L"MTQQ_TEST_VALID_CERT");
    auto transport = WindowsMqttTransportTestAccess::Create([] { return false; }, trust.engine);
    ExpectCertificateFailure(*transport, {"127.0.0.1", Port(L"MTQQ_TEST_TLS_PORT"), true}, CERT_E_CN_NO_MATCH);
}

TEST(WindowsTransportTest, SchannelRejectsExpiredCertificate) {
    TestTrust trust(L"MTQQ_TEST_EXPIRED_CERT");
    auto transport = WindowsMqttTransportTestAccess::Create([] { return false; }, trust.engine);
    ExpectCertificateFailure(*transport, {"localhost", Port(L"MTQQ_TEST_EXPIRED_PORT"), true}, CERT_E_EXPIRED);
}

TEST(WindowsTransportTest, CancelsStalledTlsHandshake) {
    std::atomic<bool> cancelled{false};
    WindowsMqttTransport transport([&] { return cancelled.load(); });
    const auto start = GetTickCount64();
    std::jthread cancel([&] { std::this_thread::sleep_for(std::chrono::milliseconds(150)); cancelled = true; });
    EXPECT_THROW(transport.Connect({"localhost", Port(L"MTQQ_TEST_STALL_PORT"), true}), std::runtime_error);
    EXPECT_LT(GetTickCount64() - start, 1500ULL);
}

TEST(WindowsTransportTest, CancelledReceiveReturnsFailureWithoutBlocking) {
    bool cancelled = false;
    WindowsMqttTransport transport([&] { return cancelled; });
    transport.Connect({"127.0.0.1", Port(L"MTQQ_TEST_TCP_PORT"), false});
    cancelled = true;
    char value;
    const auto start = GetTickCount64();
    EXPECT_EQ(transport.Receive(&value, 1), -1);
    EXPECT_LT(GetTickCount64() - start, 500ULL);
}

TEST(WindowsTransportTest, CoreMqttOverSchannelReconnectsAndResubscribes) {
    TestTrust trust(L"MTQQ_TEST_VALID_CERT");
    AppConfig config;
    config.topics = {{"integration/#", ""}, {"other/+", ""}};
    config.url = "mqtts://localhost:" + std::to_string(Port(L"MTQQ_TEST_MQTT_PORT"));
    int messages = 0;
    for (int connection = 0; connection < 2; ++connection) {
        if (connection) config.topics[0].topic = "updated/#";
        const auto start = GetTickCount64();
        MqttCancelled cancelled = [&] { return GetTickCount64() - start > 10000; };
        auto transport = WindowsMqttTransportTestAccess::Create(cancelled, trust.engine);
        transport->Connect(ParseMqttEndpoint(config.url));
        EXPECT_THROW(RunMqttSession(config, *transport, cancelled, [&](auto topic, auto payload) {
            EXPECT_EQ(topic, connection ? "updated/日本語" : "integration/日本語");
            EXPECT_EQ(payload, std::string_view("A\0B", 3));
            ++messages;
        }, {60, 2000, "integration-client"}), std::runtime_error);
        EXPECT_EQ(messages, connection + 1);
    }
}

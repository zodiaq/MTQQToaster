#include <winsock2.h>
#include <ws2tcpip.h>
#include "mqtt_client.h"
#include <core_mqtt.h>
#include <algorithm>
#include <charconv>
#include <cctype>
#include <exception>
#include <limits>
#include <stdexcept>

namespace {

bool MqttUtf8(std::string_view value)
{
    if (value.size() > UINT16_MAX || value.find('\0') != value.npos) return false;
    if (value.empty()) return true;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0) > 0;
}

void Check(MQTTStatus_t status)
{
    if (status != MQTTSuccess) throw std::runtime_error(std::string("coreMQTT: ") + MQTT_Status_strerror(status));
}

uint32_t Milliseconds() { return static_cast<uint32_t>(GetTickCount64()); }

}

MqttEndpoint ParseMqttEndpoint(std::string_view url)
{
    MqttEndpoint endpoint;
    std::string prefix(url.substr(0, 8));
    std::transform(prefix.begin(), prefix.end(), prefix.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (prefix.starts_with("mqtts://")) { endpoint.tls = true; endpoint.port = 8883; url.remove_prefix(8); }
    else if (prefix.starts_with("mqtt://")) url.remove_prefix(7);
    else throw std::invalid_argument("MQTT URL must use mqtt:// or mqtts://");
    if (url.empty() || url.find_first_of("/@?#\\ \t\r\n") != url.npos || !MqttUtf8(url)) {
        throw std::invalid_argument("MQTT URL must contain only a host and optional port");
    }
    std::string_view host, port;
    if (url.front() == '[') {
        const auto end = url.find(']');
        if (end == url.npos || end == 1) throw std::invalid_argument("Invalid IPv6 MQTT host");
        host = url.substr(1, end - 1);
        IN6_ADDR address{};
        if (InetPtonA(AF_INET6, std::string(host).c_str(), &address) != 1) throw std::invalid_argument("Brackets require an IPv6 address");
        if (end + 1 < url.size()) {
            if (url[end + 1] != ':') throw std::invalid_argument("Invalid MQTT port");
            port = url.substr(end + 2);
            if (port.empty()) throw std::invalid_argument("Empty MQTT port");
        }
    }
    else {
        const auto colon = url.find(':');
        host = url.substr(0, colon);
        if (colon != url.npos) {
            port = url.substr(colon + 1);
            if (port.empty()) throw std::invalid_argument("Empty MQTT port");
        }
    }
    if (host.empty() || host.find_first_of("[]%") != host.npos) throw std::invalid_argument("Invalid MQTT host");
    if (!port.empty()) {
        unsigned int parsed = 0;
        const auto [end, error] = std::from_chars(port.data(), port.data() + port.size(), parsed);
        if (error != std::errc{} || end != port.data() + port.size() || parsed == 0 || parsed > UINT16_MAX) {
            throw std::invalid_argument("Invalid MQTT port");
        }
        endpoint.port = static_cast<uint16_t>(parsed);
    }
    endpoint.host = host;
    return endpoint;
}

bool IsValidMqttFilter(std::string_view filter)
{
    if (filter.empty() || !MqttUtf8(filter)) return false;
    for (size_t i = 0; i < filter.size(); ++i) {
        if (filter[i] == '#' && (i + 1 != filter.size() || (i && filter[i - 1] != '/'))) return false;
        if (filter[i] == '+' && ((i && filter[i - 1] != '/') || (i + 1 < filter.size() && filter[i + 1] != '/'))) return false;
    }
    return true;
}

bool MqttTopicMatches(std::string_view filter, std::string_view topic)
{
    if (!IsValidMqttFilter(filter) || topic.empty() || !MqttUtf8(topic) || topic.find_first_of("+#") != topic.npos) return false;
    bool matches = false;
    return MQTT_MatchTopic(topic.data(), static_cast<uint16_t>(topic.size()), filter.data(),
        static_cast<uint16_t>(filter.size()), &matches) == MQTTSuccess && matches;
}

void ValidateMqttConfig(const AppConfig& config)
{
    ParseMqttEndpoint(config.url);
    if (config.topics.empty() || config.topics.size() > 128) throw std::invalid_argument("Configure between 1 and 128 MQTT topics");
    for (const auto& topic : config.topics) {
        if (!IsValidMqttFilter(topic.topic)) throw std::invalid_argument("Invalid MQTT topic filter");
    }
    if (!MqttUtf8(config.user) || config.pass.size() > UINT16_MAX || (!config.pass.empty() && config.user.empty())) {
        throw std::invalid_argument("Invalid MQTT credentials (password requires username)");
    }
}

// coreMQTT deliberately leaves this type opaque to the transport implementation.
struct NetworkContext {
    MqttTransport* transport;
    const MqttCancelled* cancelled;
    const MqttMessageHandler* onMessage;
    uint16_t pendingSubscription = 0;
    bool acknowledged = false;
    std::exception_ptr failure;
};

namespace {

int32_t Send(NetworkContext_t* context, const void* data, size_t size) noexcept
{
    try { return (*context->cancelled)() ? -1 : context->transport->Send(data, size); }
    catch (...) { context->failure = std::current_exception(); return -1; }
}

int32_t Receive(NetworkContext_t* context, void* data, size_t size) noexcept
{
    try { return (*context->cancelled)() ? -1 : context->transport->Receive(data, size); }
    catch (...) { context->failure = std::current_exception(); return -1; }
}

void Event(MQTTContext_t* mqtt, MQTTPacketInfo_t* packet, MQTTDeserializedInfo_t* info) noexcept
{
    auto& context = *mqtt->transportInterface.pNetworkContext;
    try {
        Check(info->deserializationResult);
        if ((packet->type & 0xf0) == MQTT_PACKET_TYPE_PUBLISH) {
            const auto& publish = *info->pPublishInfo;
            const std::string_view topic(publish.pTopicName, publish.topicNameLength);
            if (!MqttUtf8(topic) || topic.empty() || topic.find_first_of("+#") != topic.npos) {
                throw std::runtime_error("Broker sent an invalid MQTT topic");
            }
            if (publish.payloadLength) (*context.onMessage)(topic,
                std::string_view(static_cast<const char*>(publish.pPayload), publish.payloadLength));
        }
        else if (packet->type == MQTT_PACKET_TYPE_SUBACK) {
            uint8_t* codes = nullptr;
            size_t count = 0;
            Check(MQTT_GetSubAckStatusCodes(packet, &codes, &count));
            if (info->packetIdentifier != context.pendingSubscription || count != 1 || codes[0] != MQTTSubAckSuccessQos0) {
                throw std::runtime_error("Broker rejected MQTT subscription or returned an invalid SUBACK");
            }
            context.acknowledged = true;
        }
    }
    catch (...) { context.failure = std::current_exception(); }
}

void Process(MQTTContext_t& mqtt, NetworkContext& network)
{
    const auto status = MQTT_ProcessLoop(&mqtt);
    if (network.failure) std::rethrow_exception(network.failure);
    if (status != MQTTNeedMoreBytes) Check(status);
}

}

void RunMqttSession(const AppConfig& config, MqttTransport& transport,
                    const MqttCancelled& cancelled, const MqttMessageHandler& onMessage,
                    const MqttSessionOptions& options)
{
    ValidateMqttConfig(config);
    if (options.clientId.empty() || !MqttUtf8(options.clientId) || !options.acknowledgementTimeoutMs) {
        throw std::invalid_argument("Invalid MQTT session options");
    }
    if (cancelled()) return;
    std::vector<uint8_t> buffer(MqttMaximumPacketSize);
    NetworkContext network{&transport, &cancelled, &onMessage};
    TransportInterface_t transportInterface{};
    transportInterface.pNetworkContext = &network;
    transportInterface.send = Send;
    transportInterface.recv = Receive;
    MQTTContext_t mqtt{};
    MQTTFixedBuffer_t fixed{buffer.data(), buffer.size()};
    Check(MQTT_Init(&mqtt, &transportInterface, Milliseconds, Event, &fixed));
    MQTTConnectInfo_t connect{};
    connect.cleanSession = true;
    connect.keepAliveSeconds = options.keepAliveSeconds;
    connect.pClientIdentifier = options.clientId.c_str();
    connect.clientIdentifierLength = static_cast<uint16_t>(options.clientId.size());
    if (!config.user.empty()) {
        connect.pUserName = config.user.c_str();
        connect.userNameLength = static_cast<uint16_t>(config.user.size());
        connect.pPassword = config.pass.c_str();
        connect.passwordLength = static_cast<uint16_t>(config.pass.size());
    }
    bool sessionPresent = false;
    Check(MQTT_Connect(&mqtt, &connect, nullptr, options.acknowledgementTimeoutMs, &sessionPresent));
    for (const auto& topic : config.topics) {
        if (cancelled()) return;
        MQTTSubscribeInfo_t subscription{};
        subscription.qos = MQTTQoS0;
        subscription.pTopicFilter = topic.topic.c_str();
        subscription.topicFilterLength = static_cast<uint16_t>(topic.topic.size());
        network.pendingSubscription = MQTT_GetPacketId(&mqtt);
        network.acknowledged = false;
        Check(MQTT_Subscribe(&mqtt, &subscription, 1, network.pendingSubscription));
        const auto start = Milliseconds();
        while (!network.acknowledged) {
            if (cancelled()) return;
            Process(mqtt, network);
            if (Milliseconds() - start >= options.acknowledgementTimeoutMs) throw std::runtime_error("MQTT SUBACK timeout");
        }
    }
    spdlog::info("MQTT connected; {} subscriptions acknowledged.", config.topics.size());
    while (!cancelled()) Process(mqtt, network);
    // A cancelled transport closes promptly instead of waiting to send DISCONNECT.
    // Clean sessions are recreated, including subscriptions, on every reconnect.
}

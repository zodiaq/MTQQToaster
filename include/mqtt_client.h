#pragma once

#include "app_logic.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

struct MqttEndpoint {
    std::string host;
    uint16_t port = 1883;
    bool tls = false;
};

MqttEndpoint ParseMqttEndpoint(std::string_view url);
bool IsValidMqttFilter(std::string_view filter);
bool MqttTopicMatches(std::string_view filter, std::string_view topic);
void ValidateMqttConfig(const AppConfig& config);

using MqttCancelled = std::function<bool()>;
using MqttMessageHandler = std::function<void(std::string_view topic, std::string_view payload)>;

// A return of zero means no data yet; negative means disconnected/failed/cancelled.
// Callbacks never throw through the coreMQTT C ABI.
class MqttTransport {
public:
    virtual ~MqttTransport() = default;
    virtual int32_t Send(const void* data, size_t size) noexcept = 0;
    virtual int32_t Receive(void* data, size_t size) noexcept = 0;
};

struct MqttSessionOptions {
    uint16_t keepAliveSeconds = 60;
    uint32_t acknowledgementTimeoutMs = 5000;
    std::string clientId;
};

inline constexpr size_t MqttMaximumPacketSize = 256 * 1024;
void RunMqttSession(const AppConfig& config, MqttTransport& transport,
                    const MqttCancelled& cancelled, const MqttMessageHandler& onMessage,
                    const MqttSessionOptions& options);

class WindowsMqttTransport final : public MqttTransport {
public:
    explicit WindowsMqttTransport(MqttCancelled cancelled);
    ~WindowsMqttTransport();
    WindowsMqttTransport(const WindowsMqttTransport&) = delete;
    WindowsMqttTransport& operator=(const WindowsMqttTransport&) = delete;
    void Connect(const MqttEndpoint& endpoint);
    int32_t Send(const void* data, size_t size) noexcept override;
    int32_t Receive(void* data, size_t size) noexcept override;
    std::string LastError() const;

private:
    // Tests use a memory-only exclusive root chain engine, never the user's trust store.
    friend struct WindowsMqttTransportTestAccess;
    WindowsMqttTransport(MqttCancelled cancelled, void* certificateChainEngine);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

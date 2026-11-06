#include <winsock2.h>
#include <ws2tcpip.h>
#define SECURITY_WIN32
#define SCHANNEL_USE_BLACKLISTS
#include <security.h>
#include <winternl.h>
#include <schannel.h>
#include <wincrypt.h>
#include "mqtt_client.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <winrt/base.h>

namespace {

[[noreturn]] void Fail(const char* operation, unsigned long error)
{
    throw std::runtime_error(std::string(operation) + " failed (code " + std::to_string(error) + ")");
}

struct EventHandle {
    HANDLE value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    EventHandle() { if (!value) Fail("CreateEvent", GetLastError()); }
    ~EventHandle() { CloseHandle(value); }
};

struct NameResolution {
    EventHandle event;
    OVERLAPPED operation{};
    HANDLE cancellation = nullptr;
    PADDRINFOEXW addresses = nullptr;
    bool pending = false;
    NameResolution() { operation.hEvent = event.value; }
    ~NameResolution() {
        if (pending) {
            GetAddrInfoExCancel(&cancellation);
            // The provider still owns operation/result storage until completion.
            WaitForSingleObject(event.value, INFINITE);
        }
        if (addresses) FreeAddrInfoExW(addresses);
    }
};

struct ContextBuffer {
    void* value;
    ~ContextBuffer() { if (value) FreeContextBuffer(value); }
};

struct Certificate {
    PCCERT_CONTEXT value = nullptr;
    ~Certificate() { if (value) CertFreeCertificateContext(value); }
};

struct CertificateChain {
    PCCERT_CHAIN_CONTEXT value = nullptr;
    ~CertificateChain() { if (value) CertFreeCertificateChain(value); }
};

std::wstring ServerName(const std::string& utf8)
{
    const std::wstring name(winrt::to_hstring(utf8));
    if (name.find(L':') != name.npos) return name; // IPv6 literal, without brackets.
    const int length = IdnToAscii(IDN_USE_STD3_ASCII_RULES, name.data(), static_cast<int>(name.size()), nullptr, 0);
    if (!length) Fail("IDN host conversion", GetLastError());
    std::wstring result(static_cast<size_t>(length), L'\0');
    if (!IdnToAscii(IDN_USE_STD3_ASCII_RULES, name.data(), static_cast<int>(name.size()), result.data(), length)) {
        Fail("IDN host conversion", GetLastError());
    }
    return result;
}

}

struct WindowsMqttTransport::Impl {
    Impl(MqttCancelled value, void* chainEngine) : cancelled(std::move(value)), trustEngine(chainEngine)
    {
        SecInvalidateHandle(&credentials);
        SecInvalidateHandle(&context);
        WSADATA data{};
        const auto result = WSAStartup(MAKEWORD(2, 2), &data);
        if (result) Fail("WSAStartup", result);
    }

    ~Impl()
    {
        if (SecIsValidHandle(&context)) DeleteSecurityContext(&context);
        if (SecIsValidHandle(&credentials)) FreeCredentialsHandle(&credentials);
        if (socket != INVALID_SOCKET) closesocket(socket);
        WSACleanup();
    }

    MqttCancelled cancelled;
    HCERTCHAINENGINE trustEngine = nullptr; // Borrowed, default Windows chain engine in production.
    SOCKET socket = INVALID_SOCKET;
    CredHandle credentials{};
    CtxtHandle context{};
    SecPkgContext_StreamSizes sizes{};
    bool tls = false;
    bool ready = false;
    bool failed = false;
    std::wstring serverName;
    std::string error;
    std::vector<uint8_t> encrypted;
    std::vector<uint8_t> plaintext;
    size_t plaintextOffset = 0;
    static constexpr size_t MaximumTlsBuffer = 256 * 1024;

    void CheckCancelled() const
    {
        if (cancelled()) throw std::runtime_error("MQTT operation cancelled");
    }

    bool Wait(bool writing, ULONGLONG deadline)
    {
        while (GetTickCount64() < deadline) {
            CheckCancelled();
            fd_set read{}, write{}, exceptional{};
            FD_ZERO(&read); FD_ZERO(&write); FD_ZERO(&exceptional);
            if (writing) FD_SET(socket, &write); else FD_SET(socket, &read);
            FD_SET(socket, &exceptional);
            const auto remaining = deadline - (std::min)(deadline, GetTickCount64());
            timeval timeout{0, static_cast<long>((std::min)(remaining, 100ULL) * 1000)};
            const int result = select(0, &read, &write, &exceptional, &timeout);
            if (result == SOCKET_ERROR) Fail("Socket wait", WSAGetLastError());
            if (result > 0) return true;
        }
        CheckCancelled();
        return false;
    }

    void SendRaw(const void* data, size_t length, ULONGLONG deadline)
    {
        size_t sent = 0;
        while (sent < length) {
            CheckCancelled();
            if (GetTickCount64() >= deadline) throw std::runtime_error("MQTT send timeout");
            const int result = send(socket, static_cast<const char*>(data) + sent,
                static_cast<int>((std::min)(length - sent, static_cast<size_t>(INT_MAX))), 0);
            if (result > 0) { sent += static_cast<size_t>(result); continue; }
            const auto code = WSAGetLastError();
            if (result == 0 || code != WSAEWOULDBLOCK) Fail("Socket send", code);
            if (!Wait(true, deadline)) throw std::runtime_error("MQTT send timeout");
        }
    }

    int ReceiveRaw(void* data, size_t length, ULONGLONG deadline)
    {
        CheckCancelled();
        if (!Wait(false, deadline)) return 0;
        const auto result = recv(socket, static_cast<char*>(data), static_cast<int>((std::min)(length, static_cast<size_t>(INT_MAX))), 0);
        if (result > 0) return result;
        if (result == 0) throw std::runtime_error("MQTT peer closed the connection");
        const auto code = WSAGetLastError();
        if (code == WSAEWOULDBLOCK) return 0;
        Fail("Socket receive", code);
    }

    bool ReadEncrypted(ULONGLONG deadline)
    {
        std::array<uint8_t, 16384> input{};
        const auto count = ReceiveRaw(input.data(), input.size(), deadline);
        if (!count) return false;
        if (encrypted.size() + static_cast<size_t>(count) > MaximumTlsBuffer) throw std::runtime_error("TLS input exceeds size limit");
        encrypted.insert(encrypted.end(), input.begin(), input.begin() + count);
        return true;
    }

    void KeepExtra(const SecBuffer* buffers, size_t count)
    {
        size_t extra = 0;
        for (size_t i = 0; i < count; ++i) {
            if (buffers[i].BufferType == SECBUFFER_EXTRA) extra = buffers[i].cbBuffer;
        }
        if (extra > encrypted.size()) throw std::runtime_error("Invalid TLS remaining data");
        encrypted.erase(encrypted.begin(), encrypted.end() - static_cast<ptrdiff_t>(extra));
    }

    void VerifyPeer()
    {
        Certificate certificate;
        auto status = QueryContextAttributesW(&context, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &certificate.value);
        if (status != SEC_E_OK) Fail("TLS peer certificate", status);
        CERT_CHAIN_PARA parameters{sizeof(parameters)};
        LPSTR usage = const_cast<LPSTR>(szOID_PKIX_KP_SERVER_AUTH);
        parameters.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
        parameters.RequestedUsage.Usage = {1, &usage};
        parameters.dwUrlRetrievalTimeout = 2000;
        CertificateChain chain;
        constexpr DWORD flags = CERT_CHAIN_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT | CERT_CHAIN_REVOCATION_ACCUMULATIVE_TIMEOUT;
        if (!CertGetCertificateChain(trustEngine, certificate.value, nullptr, certificate.value->hCertStore,
                &parameters, flags, nullptr, &chain.value)) Fail("TLS certificate chain", GetLastError());
        SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl{sizeof(ssl)};
        ssl.dwAuthType = AUTHTYPE_SERVER;
        ssl.pwszServerName = serverName.data();
        CERT_CHAIN_POLICY_PARA policy{sizeof(policy)};
        policy.pvExtraPolicyPara = &ssl;
        CERT_CHAIN_POLICY_STATUS result{sizeof(result)};
        if (!CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain.value, &policy, &result)) {
            Fail("TLS certificate policy", GetLastError());
        }
        if (result.dwError) Fail("TLS certificate validation", result.dwError);
        CheckCancelled();
    }

    void Handshake()
    {
        const auto deadline = GetTickCount64() + 10000;
        bool needRead = SecIsValidHandle(&context) && encrypted.empty();
        while (true) {
            CheckCancelled();
            if (GetTickCount64() >= deadline) throw std::runtime_error("TLS handshake timeout");
            if (needRead && !ReadEncrypted(deadline)) throw std::runtime_error("TLS handshake timeout");
            SecBuffer input[] = {{static_cast<unsigned long>(encrypted.size()), SECBUFFER_TOKEN, encrypted.data()},
                                 {0, SECBUFFER_EMPTY, nullptr}};
            SecBufferDesc inputDescription{SECBUFFER_VERSION, 2, input};
            SecBuffer output{0, SECBUFFER_TOKEN, nullptr};
            SecBufferDesc outputDescription{SECBUFFER_VERSION, 1, &output};
            unsigned long attributes = 0;
            TimeStamp expiry{};
            constexpr unsigned long requested = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                ISC_REQ_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | ISC_REQ_MANUAL_CRED_VALIDATION;
            const bool continuing = SecIsValidHandle(&context);
            const auto status = InitializeSecurityContextW(&credentials, continuing ? &context : nullptr,
                serverName.data(), requested, 0, SECURITY_NATIVE_DREP, continuing ? &inputDescription : nullptr,
                0, &context, &outputDescription, &attributes, &expiry);
            const ContextBuffer outputOwner{output.pvBuffer};
            if (status == SEC_E_INCOMPLETE_MESSAGE) { needRead = true; continue; }
            if (status != SEC_E_OK && status != SEC_I_CONTINUE_NEEDED) Fail("TLS handshake", status);
            if (output.cbBuffer) SendRaw(output.pvBuffer, output.cbBuffer, deadline);
            if (continuing) KeepExtra(input, std::size(input));
            if (status == SEC_E_OK) {
                if (!(attributes & ISC_RET_CONFIDENTIALITY)) throw std::runtime_error("TLS confidentiality was not negotiated");
                VerifyPeer();
                const auto sizeStatus = QueryContextAttributesW(&context, SECPKG_ATTR_STREAM_SIZES, &sizes);
                if (sizeStatus != SEC_E_OK) Fail("TLS stream sizes", sizeStatus);
                return;
            }
            needRead = encrypted.empty();
        }
    }

    void Connect(const MqttEndpoint& endpoint)
    {
        if (socket != INVALID_SOCKET) throw std::logic_error("Transport is already connected");
        CheckCancelled();
        serverName = ServerName(endpoint.host);
        NameResolution resolution;
        ADDRINFOEXW hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        timeval timeout{5, 0};
        const auto service = std::to_wstring(endpoint.port);
        int result = GetAddrInfoExW(serverName.c_str(), service.c_str(), NS_DNS, nullptr, &hints,
            &resolution.addresses, &timeout, &resolution.operation, nullptr, &resolution.cancellation);
        if (result == WSA_IO_PENDING) {
            resolution.pending = true;
            const auto deadline = GetTickCount64() + 5000;
            DWORD waitResult;
            while ((waitResult = WaitForSingleObject(resolution.event.value, 100)) == WAIT_TIMEOUT) {
                CheckCancelled();
                if (GetTickCount64() >= deadline) throw std::runtime_error("MQTT DNS timeout");
            }
            if (waitResult != WAIT_OBJECT_0) Fail("MQTT DNS wait", GetLastError());
            result = GetAddrInfoExOverlappedResult(&resolution.operation);
            resolution.pending = false;
        }
        if (result) Fail("MQTT name resolution", result);
        const auto deadline = GetTickCount64() + 5000;
        bool connected = false;
        for (auto address = resolution.addresses; address; address = address->ai_next) {
            CheckCancelled();
            if (socket != INVALID_SOCKET) closesocket(socket);
            socket = ::socket(address->ai_family, SOCK_STREAM, IPPROTO_TCP);
            if (socket == INVALID_SOCKET) continue;
            u_long nonblocking = 1;
            if (ioctlsocket(socket, FIONBIO, &nonblocking)) Fail("Nonblocking socket", WSAGetLastError());
            result = connect(socket, address->ai_addr, static_cast<int>(address->ai_addrlen));
            if (result == 0) { connected = true; break; }
            if (WSAGetLastError() != WSAEWOULDBLOCK) continue;
            if (!Wait(true, deadline)) break;
            int errorCode = 0, length = sizeof(errorCode);
            if (getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&errorCode), &length)) Fail("Socket status", WSAGetLastError());
            if (!errorCode) { connected = true; break; }
        }
        if (!connected) throw std::runtime_error("MQTT TCP connection failed or timed out");
        tls = endpoint.tls;
        if (tls) {
            TLS_PARAMETERS parameters{};
            parameters.grbitDisabledProtocols = SP_PROT_SSL2 | SP_PROT_SSL3 | SP_PROT_TLS1_0 | SP_PROT_TLS1_1;
            SCH_CREDENTIALS settings{};
            settings.dwVersion = SCH_CREDENTIALS_VERSION;
            // Validation is performed explicitly by VerifyPeer before any MQTT credentials are sent.
            settings.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS;
            settings.cTlsParameters = 1;
            settings.pTlsParameters = &parameters;
            TimeStamp expiry{};
            const auto status = AcquireCredentialsHandleW(nullptr, const_cast<wchar_t*>(UNISP_NAME_W), SECPKG_CRED_OUTBOUND,
                nullptr, &settings, nullptr, nullptr, &credentials, &expiry);
            if (status != SEC_E_OK) Fail("Schannel credentials", status);
            Handshake();
        }
        CheckCancelled();
        ready = true;
    }

    int32_t Send(const void* data, size_t length)
    {
        CheckCancelled();
        if (!length) return 0;
        const auto deadline = GetTickCount64() + 5000;
        if (!tls) {
            const auto count = (std::min)(length, static_cast<size_t>(INT32_MAX));
            SendRaw(data, count, deadline);
            return static_cast<int32_t>(count);
        }
        const auto count = (std::min)(length, static_cast<size_t>(sizes.cbMaximumMessage));
        std::vector<uint8_t> record(sizes.cbHeader + count + sizes.cbTrailer);
        std::memcpy(record.data() + sizes.cbHeader, data, count);
        SecBuffer buffers[] = {{sizes.cbHeader, SECBUFFER_STREAM_HEADER, record.data()},
            {static_cast<unsigned long>(count), SECBUFFER_DATA, record.data() + sizes.cbHeader},
            {sizes.cbTrailer, SECBUFFER_STREAM_TRAILER, record.data() + sizes.cbHeader + count},
            {0, SECBUFFER_EMPTY, nullptr}};
        SecBufferDesc description{SECBUFFER_VERSION, 4, buffers};
        const auto status = EncryptMessage(&context, 0, &description, 0);
        if (status != SEC_E_OK) Fail("TLS encryption", status);
        for (size_t i = 0; i < 3; ++i) SendRaw(buffers[i].pvBuffer, buffers[i].cbBuffer, deadline);
        return static_cast<int32_t>(count);
    }

    int32_t Receive(void* data, size_t length)
    {
        CheckCancelled();
        if (!length) return 0;
        const auto deadline = GetTickCount64() + 100;
        if (!tls) return ReceiveRaw(data, length, deadline);
        while (plaintextOffset == plaintext.size()) {
            plaintext.clear();
            plaintextOffset = 0;
            if (encrypted.empty() && !ReadEncrypted(deadline)) return 0;
            SecBuffer buffers[] = {{static_cast<unsigned long>(encrypted.size()), SECBUFFER_DATA, encrypted.data()},
                {0, SECBUFFER_EMPTY, nullptr}, {0, SECBUFFER_EMPTY, nullptr}, {0, SECBUFFER_EMPTY, nullptr}};
            SecBufferDesc description{SECBUFFER_VERSION, 4, buffers};
            const auto status = DecryptMessage(&context, &description, 0, nullptr);
            if (status == SEC_E_INCOMPLETE_MESSAGE) { if (!ReadEncrypted(deadline)) return 0; continue; }
            if (status != SEC_E_OK && status != SEC_I_RENEGOTIATE) Fail("TLS decryption", status);
            for (const auto& buffer : buffers) {
                if (buffer.BufferType == SECBUFFER_DATA && buffer.cbBuffer) {
                    const auto begin = static_cast<const uint8_t*>(buffer.pvBuffer);
                    plaintext.insert(plaintext.end(), begin, begin + buffer.cbBuffer);
                }
            }
            KeepExtra(buffers, std::size(buffers));
            if (status == SEC_I_RENEGOTIATE) Handshake();
            if (plaintext.empty() && GetTickCount64() >= deadline) return 0;
        }
        const auto count = (std::min)(length, plaintext.size() - plaintextOffset);
        std::memcpy(data, plaintext.data() + plaintextOffset, count);
        plaintextOffset += count;
        return static_cast<int32_t>(count);
    }

    template<class Operation> int32_t Guard(Operation&& operation) noexcept
    {
        if (failed || !ready) return -1;
        try { return operation(); }
        catch (const std::exception& ex) { try { error = ex.what(); } catch (...) {} }
        catch (...) { try { error = "MQTT transport failure"; } catch (...) {} }
        failed = true;
        return -1;
    }
};

WindowsMqttTransport::WindowsMqttTransport(MqttCancelled cancelled) : WindowsMqttTransport(std::move(cancelled), nullptr) {}
WindowsMqttTransport::WindowsMqttTransport(MqttCancelled cancelled, void* engine) : impl_(std::make_unique<Impl>(std::move(cancelled), engine)) {}
WindowsMqttTransport::~WindowsMqttTransport() = default;
void WindowsMqttTransport::Connect(const MqttEndpoint& endpoint) { impl_->Connect(endpoint); }
int32_t WindowsMqttTransport::Send(const void* data, size_t size) noexcept { return impl_->Guard([&] { return impl_->Send(data, size); }); }
int32_t WindowsMqttTransport::Receive(void* data, size_t size) noexcept { return impl_->Guard([&] { return impl_->Receive(data, size); }); }
std::string WindowsMqttTransport::LastError() const { return impl_->error; }

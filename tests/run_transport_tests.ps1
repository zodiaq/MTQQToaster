param([Parameter(Mandatory)][string]$TestExecutable)
$ErrorActionPreference = 'Stop'

# Local-only fixtures: no certificate installation, broker account, or external network.
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Net.Security;
using System.Security.Authentication;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

public sealed class MqttTransportFixture : IDisposable {
    readonly CancellationTokenSource stop = new CancellationTokenSource();
    readonly List<TcpListener> listeners = new List<TcpListener>();
    readonly List<Task> tasks = new List<Task>();
    readonly List<X509Certificate2> certificates = new List<X509Certificate2>();
    public X509Certificate2 Certificate(bool expired) {
        using (RSA key = RSA.Create(2048)) {
            var request = new CertificateRequest("CN=localhost", key, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
            request.CertificateExtensions.Add(new X509BasicConstraintsExtension(true, false, 0, true));
            request.CertificateExtensions.Add(new X509KeyUsageExtension(X509KeyUsageFlags.DigitalSignature | X509KeyUsageFlags.KeyCertSign, true));
            request.CertificateExtensions.Add(new X509EnhancedKeyUsageExtension(new OidCollection { new Oid("1.3.6.1.5.5.7.3.1") }, false));
            var san = new SubjectAlternativeNameBuilder(); san.AddDnsName("localhost");
            request.CertificateExtensions.Add(san.Build());
            var now = DateTimeOffset.UtcNow;
            using (var temporary = request.CreateSelfSigned(now.AddDays(-2), expired ? now.AddDays(-1) : now.AddDays(1))) {
                // Windows server Schannel needs a temporary key container. Do not use PersistKeySet:
                // disposing the certificate removes that container; no trust store is modified.
                var certificate = X509CertificateLoader.LoadPkcs12(temporary.Export(X509ContentType.Pfx), null, X509KeyStorageFlags.DefaultKeySet);
                certificates.Add(certificate);
                return certificate;
            }
        }
    }
    public int Listen(X509Certificate2 certificate, bool stall) { return Start(certificate, stall, false); }
    public int ListenMqtt(X509Certificate2 certificate) { return Start(certificate, false, true); }
    int Start(X509Certificate2 certificate, bool stall, bool mqtt) {
        var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start(); listeners.Add(listener);
        tasks.Add(Task.Run(async () => {
            try {
                while (!stop.IsCancellationRequested) {
                    var client = await listener.AcceptTcpClientAsync(stop.Token);
                    var task = Serve(client, certificate, stall, mqtt);
                    lock (tasks) tasks.Add(task);
                }
            } catch (OperationCanceledException) {} catch (SocketException) {}
        }));
        return ((IPEndPoint)listener.LocalEndpoint).Port;
    }
    async Task Serve(TcpClient client, X509Certificate2 certificate, bool stall, bool mqtt) {
        using (client) {
            try {
                if (stall) { await Task.Delay(Timeout.Infinite, stop.Token); return; }
                Stream stream = client.GetStream();
                if (certificate != null) {
                    var ssl = new SslStream(stream, false);
                    stream = ssl;
                    await ssl.AuthenticateAsServerAsync(new SslServerAuthenticationOptions {
                        ServerCertificate = certificate, EnabledSslProtocols = SslProtocols.Tls12 | SslProtocols.Tls13,
                        ClientCertificateRequired = false
                    }, stop.Token);
                }
                using (stream) {
                    if (mqtt) { await Broker(stream); return; }
                    byte[] buffer = new byte[4096];
                    int count;
                    while ((count = await stream.ReadAsync(buffer.AsMemory(), stop.Token)) > 0) {
                        // Many small TLS records and combined TCP reads exercise leftover handling.
                        for (int offset = 0; offset < count; offset += 113)
                            await stream.WriteAsync(buffer.AsMemory(offset, Math.Min(113, count - offset)), stop.Token);
                    }
                }
            } catch (AuthenticationException error) { Console.Error.WriteLine("Test TLS server: " + error); }
              catch (IOException) {} catch (OperationCanceledException) {} catch (SocketException) {}
        }
    }
    async Task<int> Byte(Stream stream) {
        byte[] value = new byte[1];
        if (await stream.ReadAsync(value.AsMemory(), stop.Token) != 1) throw new IOException("Client closed");
        return value[0];
    }
    async Task Packet(Stream stream, byte type, byte[] body) {
        await stream.WriteAsync(new byte[] { type }.AsMemory(), stop.Token);
        int length = body.Length;
        do {
            byte digit = (byte)(length % 128); length /= 128;
            if (length != 0) digit |= 128;
            await stream.WriteAsync(new byte[] { digit }.AsMemory(), stop.Token);
        } while (length != 0);
        for (int offset = 0; offset < body.Length; offset += 3)
            await stream.WriteAsync(body.AsMemory(offset, Math.Min(3, body.Length - offset)), stop.Token);
    }
    async Task Broker(Stream stream) {
        int subscriptions = 0;
        string topic = "";
        while (!stop.IsCancellationRequested) {
            int type = await Byte(stream), length = 0, multiplier = 1, digit;
            do {
                digit = await Byte(stream); length += (digit & 127) * multiplier; multiplier *= 128;
                if (multiplier > 2097152 || length > 262144) throw new IOException("Oversized test request");
            } while ((digit & 128) != 0);
            byte[] body = new byte[length];
            await stream.ReadExactlyAsync(body.AsMemory(), stop.Token);
            if (type == 0x10) await Packet(stream, 0x20, new byte[] { 0, 0 });
            else if (type == 0x82) {
                if (body.Length < 5) throw new IOException("Invalid test subscription");
                if (subscriptions++ == 0) topic = Encoding.UTF8.GetString(body, 4, body[2] * 256 + body[3]).Replace("#", "日本語").Replace("+", "日本語");
                await Packet(stream, 0x90, new byte[] { body[0], body[1], 0 });
                if (subscriptions == 2) {
                    byte[] name = Encoding.UTF8.GetBytes(topic);
                    byte[] publish = new byte[2 + name.Length + 3];
                    publish[0] = (byte)(name.Length >> 8); publish[1] = (byte)name.Length;
                    name.CopyTo(publish, 2);
                    publish[publish.Length - 3] = 65; publish[publish.Length - 2] = 0; publish[publish.Length - 1] = 66;
                    await Packet(stream, 0x30, publish);
                    return; // Deliberate broker disconnect exercises reconnect/resubscribe.
                }
            } else if (type == 0xc0) await Packet(stream, 0xd0, Array.Empty<byte>());
        }
    }
    public void Dispose() {
        stop.Cancel();
        foreach (var listener in listeners) listener.Stop();
        Task[] pending; lock (tasks) pending = tasks.ToArray();
        try { Task.WaitAll(pending, 5000); } catch (AggregateException) {}
        foreach (var certificate in certificates) certificate.Dispose();
        stop.Dispose();
    }
}
'@

$fixture = [MqttTransportFixture]::new()
$directory = Join-Path ([IO.Path]::GetTempPath()) ('MTQQToaster-tls-' + [guid]::NewGuid().ToString('N'))
$savedEnvironment = @{}
$variables = @('MTQQ_TEST_TCP_PORT', 'MTQQ_TEST_TLS_PORT', 'MTQQ_TEST_MQTT_PORT', 'MTQQ_TEST_EXPIRED_PORT', 'MTQQ_TEST_STALL_PORT', 'MTQQ_TEST_VALID_CERT', 'MTQQ_TEST_EXPIRED_CERT')
foreach ($variable in $variables) { $savedEnvironment[$variable] = [Environment]::GetEnvironmentVariable($variable) }
$exitCode = 1
try {
    New-Item -ItemType Directory -Path $directory | Out-Null
    $valid = $fixture.Certificate($false)
    $expired = $fixture.Certificate($true)
    $env:MTQQ_TEST_VALID_CERT = Join-Path $directory 'valid.cer'
    $env:MTQQ_TEST_EXPIRED_CERT = Join-Path $directory 'expired.cer'
    [IO.File]::WriteAllBytes($env:MTQQ_TEST_VALID_CERT, $valid.Export([Security.Cryptography.X509Certificates.X509ContentType]::Cert))
    [IO.File]::WriteAllBytes($env:MTQQ_TEST_EXPIRED_CERT, $expired.Export([Security.Cryptography.X509Certificates.X509ContentType]::Cert))
    $env:MTQQ_TEST_TCP_PORT = $fixture.Listen($null, $false)
    $env:MTQQ_TEST_TLS_PORT = $fixture.Listen($valid, $false)
    $env:MTQQ_TEST_MQTT_PORT = $fixture.ListenMqtt($valid)
    $env:MTQQ_TEST_EXPIRED_PORT = $fixture.Listen($expired, $false)
    $env:MTQQ_TEST_STALL_PORT = $fixture.Listen($null, $true)
    & $TestExecutable
    $exitCode = $LASTEXITCODE
}
finally {
    $fixture.Dispose()
    foreach ($name in @('valid.cer', 'expired.cer')) {
        $path = Join-Path $directory $name
        if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
    }
    if (Test-Path -LiteralPath $directory) { Remove-Item -LiteralPath $directory }
    foreach ($variable in $variables) { [Environment]::SetEnvironmentVariable($variable, $savedEnvironment[$variable]) }
}
exit $exitCode

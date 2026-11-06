#requires -Version 5.1
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path $PSScriptRoot -Parent

& (Join-Path $PSScriptRoot 'package-msix.ps1')

# Keep the private key in the current user's personal store, never in the ZIP.
$subject = 'CN=MTQQToaster Development'
$cert = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert |
    Where-Object { $_.Subject -eq $subject -and $_.HasPrivateKey -and $_.NotAfter -gt (Get-Date).AddDays(30) } |
    Sort-Object NotAfter -Descending | Select-Object -First 1
if (!$cert) {
    $cert = New-SelfSignedCertificate -Type Custom -Subject $subject `
        -FriendlyName 'MTQQToaster development package signing' `
        -KeyUsage DigitalSignature -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 `
        -KeyExportPolicy NonExportable -CertStoreLocation Cert:\CurrentUser\My `
        -NotAfter (Get-Date).AddYears(1) `
        -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}')
}
$sdkRoot = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots').KitsRoot10
$sdk = Get-ChildItem (Join-Path $sdkRoot 'bin') -Directory |
    Where-Object { $_.Name -match '^10\.0\.\d+\.\d+$' -and (Test-Path (Join-Path $_.FullName 'x64/SignTool.exe')) } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (!$sdk) { throw 'Windows SDK SignTool is required.' }
$destination = Join-Path $root ('out/setup/' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $destination -Force | Out-Null
$packageName = 'MTQQToaster_1.0.0.0_x64.msix'
$package = Join-Path $destination $packageName
Copy-Item -LiteralPath (Join-Path $root "out/msix/$packageName") -Destination $package
& (Join-Path $sdk.FullName 'x64/SignTool.exe') sign /fd SHA256 /sha1 $cert.Thumbprint $package
if ($LASTEXITCODE) { throw 'Package signing failed.' }
Export-Certificate -Cert $cert -FilePath (Join-Path $destination 'MTQQToaster-Development.cer') | Out-Null

# Verify the embedded CMS signature without changing the machine's trust stores.
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName System.Security
$zip = [System.IO.Compression.ZipFile]::OpenRead($package)
try {
    $entry = $zip.GetEntry('AppxSignature.p7x')
    if (!$entry) { throw 'Package signature is missing.' }
    $stream = $entry.Open()
    $buffer = [System.IO.MemoryStream]::new()
    try { $stream.CopyTo($buffer); $bytes = $buffer.ToArray() }
    finally { $stream.Dispose(); $buffer.Dispose() }
    $cms = [System.Security.Cryptography.Pkcs.SignedCms]::new()
    $cms.Decode([byte[]]$bytes[4..($bytes.Length - 1)])
    $cms.CheckSignature($true)
    if ($cms.SignerInfos.Count -ne 1 -or $cms.SignerInfos[0].Certificate.Thumbprint -ne $cert.Thumbprint) {
        throw 'Unexpected package signer.'
    }
} finally { $zip.Dispose() }

$instructions = @"
MTQQToaster 1.0.0.0 x64 開発用セットアップ

Windows 10 version 1809以降 / Windows 11 x64

1. ZIPを展開します。
2. MTQQToaster-Development.cerを開き、「証明書のインストール」を選びます。
   保存場所は「ローカル コンピューター」、ストアは「信頼されたユーザー
   (Trusted People)」を明示して選択します。管理者権限が必要です。
   この操作により、この開発証明書で署名したパッケージが信頼されます。
3. $packageName をダブルクリックし、インストールします。
4. スタートメニューからMTQQToasterを起動します。
   EXE版を利用している場合は、先にその自動起動を解除して終了してください。

署名証明書の拇印: $($cert.Thumbprint)
証明書期限: $($cert.NotAfter.ToString('yyyy-MM-dd'))
開発用の自己署名です。一般公開用の信頼された発行者証明書ではありません。
秘密鍵は同梱していません。証明書登録とインストールは自動実行しません。
アンインストールはWindows設定のアプリ一覧から行います。
設定の退避後にアンインストールしてください。
"@
Set-Content -LiteralPath (Join-Path $destination 'インストール手順.txt') -Value $instructions -Encoding UTF8
$archive = Join-Path $root 'out/setup/MTQQToaster-1.0.0.0-x64-setup.zip'
Compress-Archive -LiteralPath $package, (Join-Path $destination 'MTQQToaster-Development.cer'), (Join-Path $destination 'インストール手順.txt') -DestinationPath $archive -Force
Write-Host "Setup ZIP: $archive"
Write-Host "CMS signature verified; certificate trust and installation are not performed."

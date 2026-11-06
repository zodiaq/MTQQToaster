#requires -Version 5.1
[CmdletBinding()]
param(
    [ValidatePattern('^[A-Za-z0-9.-]{3,50}$')][string]$IdentityName = 'MTQQToaster',
    [ValidateNotNullOrEmpty()][string]$Publisher = 'CN=MTQQToaster Development',
    [ValidateNotNullOrEmpty()][string]$PublisherDisplayName = 'MTQQToaster',
    [ValidatePattern('^[1-9][0-9]*\.[0-9]+\.[0-9]+\.[0-9]+$')][string]$Version = (& (Join-Path $PSScriptRoot 'get-version.ps1'))
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path $PSScriptRoot -Parent
foreach ($part in $Version.Split('.')) {
    if ([uint64]$part -gt 65535) { throw 'Each version component must be <= 65535.' }
}
$sdkRoot = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots').KitsRoot10
$sdk = Get-ChildItem (Join-Path $sdkRoot 'bin') -Directory |
    Where-Object { $_.Name -match '^10\.0\.\d+\.\d+$' -and (Test-Path (Join-Path $_.FullName 'x64/MakeAppx.exe')) } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (!$sdk) { throw 'Install Windows SDK with MakeAppx.exe.' }

Push-Location $root
try {
    & cmake --preset x64-release
    if ($LASTEXITCODE) { throw 'CMake configure failed.' }
    & cmake --build --preset x64-release --parallel
    if ($LASTEXITCODE) { throw 'Release build failed.' }
    & ctest --preset x64-release
    if ($LASTEXITCODE) { throw 'Release tests failed.' }

    # Each invocation gets an isolated staging directory; never package user config or logs.
    $output = Join-Path $root 'out/msix'
    $stage = Join-Path $output ('stage-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path (Join-Path $stage 'Assets') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $stage 'Licenses') | Out-Null
    Copy-Item -LiteralPath (Join-Path $root 'out/build/windows-x64/Release/MTQQToaster.exe') -Destination $stage
    Copy-Item -LiteralPath (Join-Path $root 'config.sample.json'), (Join-Path $root 'LICENSE.txt') -Destination $stage
    foreach ($dependency in @('coremqtt', 'spdlog', 'fmt', 'nlohmann-json')) {
        $copyright = Join-Path $root "out/build/windows-x64/vcpkg_installed/x64-windows-static/share/$dependency/copyright"
        Copy-Item -LiteralPath $copyright -Destination (Join-Path $stage "Licenses/$dependency.txt")
    }

    [xml]$manifest = Get-Content -LiteralPath (Join-Path $root 'packaging/AppxManifest.xml') -Raw
    $manifest.Package.Identity.Name = $IdentityName
    $manifest.Package.Identity.Publisher = $Publisher
    $manifest.Package.Identity.Version = $Version
    $manifest.Package.Properties.PublisherDisplayName = $PublisherDisplayName
    $manifest.Save((Join-Path $stage 'AppxManifest.xml'))

    # Preserve artwork transparency and supply high-DPI assets instead of upscaling small logos.
    Add-Type -AssemblyName System.Drawing
    foreach ($asset in @(@('StoreLogo', 50), @('Square44x44Logo', 44), @('Square150x150Logo', 150))) {
      foreach ($scale in @(100, 200, 400)) {
        $size = [int]($asset[1] * $scale / 100)
        $icon = [System.Drawing.Image]::FromFile((Join-Path $root 'resources/package-icon.png'))
        try {
            $bitmap = [System.Drawing.Bitmap]::new($size, $size)
            try {
                $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
                try {
                    $graphics.Clear([System.Drawing.Color]::Transparent)
                    $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
                    $graphics.DrawImage($icon, [System.Drawing.Rectangle]::new(0, 0, $size, $size))
                } finally { $graphics.Dispose() }
                $suffix = if ($scale -eq 100) { '' } else { ".scale-$scale" }
                $bitmap.Save((Join-Path $stage ('Assets/' + $asset[0] + $suffix + '.png')), [System.Drawing.Imaging.ImageFormat]::Png)
            } finally { $bitmap.Dispose() }
        } finally { $icon.Dispose() }
      }
    }
    $package = Join-Path $output "$IdentityName`_$Version`_x64.msix"
    & (Join-Path $sdk.FullName 'x64/MakeAppx.exe') pack /d $stage /p $package /o
    if ($LASTEXITCODE) { throw 'MakeAppx validation/packaging failed.' }
    Write-Host "Unsigned package: $package"
    Write-Host "Staging directory: $stage"
} finally { Pop-Location }

#requires -Version 5.1
$root = Split-Path $PSScriptRoot -Parent
$cmake = Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw
if ($cmake -notmatch 'project\s*\("MTQQToaster"\s+VERSION\s+([1-9][0-9]*\.[0-9]+\.[0-9]+\.[0-9]+)\s+LANGUAGES') {
    throw 'CMakeLists.txt must declare a four-component MTQQToaster version.'
}
$version = $Matches[1]
foreach ($part in $version.Split('.')) {
    if ([uint64]$part -gt 65535) { throw 'Each version component must be <= 65535.' }
}
$version

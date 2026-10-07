<#
.SYNOPSIS
  Wrap the MSI cpack just built in a Burn bootstrapper .exe.

.DESCRIPTION
  A .msi cannot carry an icon, so the file people download is this .exe. See
  cmake/bundle.wxs.in.

  Run after cpack, against the same build directory. CMake has already written
  bundle.wxs there with the version, the paths and the MSI's name in it, so all
  that is left is candle and light. Needs the WiX Toolset v3 on PATH or under
  %WIX%.
#>
[CmdletBinding()]
param(
    # The build directory cpack was run against, e.g. build.
    [Parameter(Mandatory = $true)][string]$BuildDir
)

$ErrorActionPreference = "Stop"

$buildPath = (Resolve-Path $BuildDir).Path
$source = Join-Path $buildPath "bundle.wxs"
if (-not (Test-Path $source)) {
    throw "No bundle.wxs in $buildPath. cmake/Packaging.cmake writes it at configure time on Windows."
}

# light.exe reports a missing SourceFile several lines deep; say it here.
# @() so one match is still an array.
$msi = @(Get-ChildItem -Path $buildPath -Filter "SoundSplice-*.msi" -File)
if ($msi.Count -ne 1) {
    throw "Expected exactly one SoundSplice-*.msi in $buildPath, found $($msi.Count). Run cpack first."
}

$candle = "candle.exe"
$light = "light.exe"
if (-not (Get-Command $candle -ErrorAction SilentlyContinue) -and $env:WIX) {
    $candle = Join-Path $env:WIX "bin\candle.exe"
    $light = Join-Path $env:WIX "bin\light.exe"
}

# The .exe takes the MSI's name, so the two are obviously the same build.
$output = Join-Path $buildPath ($msi[0].BaseName + ".exe")
$object = Join-Path $buildPath "bundle.wixobj"

# WixBalExtension is where BootstrapperApplicationRef and the bal: namespace
# come from.
Write-Host "candle: $source"
& $candle -nologo -ext WixBalExtension -out $object $source
if ($LASTEXITCODE -ne 0) { throw "candle.exe failed with $LASTEXITCODE" }

Write-Host "light: $output"
& $light -nologo -ext WixBalExtension -out $output $object
if ($LASTEXITCODE -ne 0) { throw "light.exe failed with $LASTEXITCODE" }

if (-not (Test-Path $output)) {
    throw "light.exe reported success but wrote no $output"
}
Write-Host "wrote $output ($((Get-Item $output).Length) bytes)"

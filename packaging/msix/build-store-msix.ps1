param(
  [Parameter(Mandatory = $true)]
  [string]$PortableDir,

  [Parameter(Mandatory = $true)]
  [string]$LogoPath,

  [Parameter(Mandatory = $true)]
  [string]$OutputDir,

  [string]$IdentityConfigPath = ""
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$templatePath = Join-Path $PSScriptRoot "AppxManifest.xml.in"

if (-not (Test-Path -LiteralPath $PortableDir)) {
  throw "PortableDir not found: $PortableDir"
}
if (-not (Test-Path -LiteralPath $LogoPath)) {
  throw "LogoPath not found: $LogoPath"
}
if (-not (Test-Path -LiteralPath $templatePath)) {
  throw "Manifest template not found: $templatePath"
}

$cmakeText = Get-Content -LiteralPath (Join-Path $repoRoot "CMakeLists.txt") -Raw
if ($cmakeText -notmatch 'project\(CommunityClashStreamer\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
  throw "Could not determine project version from CMakeLists.txt"
}
$packageVersion = "$($Matches[1]).0"

$storeReady = $false
if ($IdentityConfigPath -and (Test-Path -LiteralPath $IdentityConfigPath)) {
  $identity = Get-Content -LiteralPath $IdentityConfigPath -Raw | ConvertFrom-Json
  $identityName = [string]$identity.identityName
  $publisher = [string]$identity.publisher
  $publisherDisplayName = [string]$identity.publisherDisplayName

  if ([string]::IsNullOrWhiteSpace($identityName) -or
      [string]::IsNullOrWhiteSpace($publisher) -or
      [string]::IsNullOrWhiteSpace($publisherDisplayName)) {
    throw "store-identity.json exists but one or more required values are empty."
  }

  if ($identityName -like "PASTE *" -or
      $publisher -like "PASTE *" -or
      $publisherDisplayName -like "PASTE *") {
    throw "store-identity.json still contains placeholder values."
  }

  $storeReady = $true
} else {
  $identityName = "CommunityClash.Streamer.Dev"
  $publisher = "CN=CommunityClashDev"
  $publisherDisplayName = "Community Clash Development"
}

New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
$outputDirResolved = (Resolve-Path -LiteralPath $OutputDir).Path
$layout = Join-Path $outputDirResolved "layout"

if (Test-Path -LiteralPath $layout) {
  Remove-Item -LiteralPath $layout -Recurse -Force
}
New-Item -ItemType Directory -Path $layout -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $layout "Assets") -Force | Out-Null

Copy-Item -Path (Join-Path $PortableDir "*") -Destination $layout -Recurse -Force

$requiredFiles = @(
  "CommunityClashStreamer.exe",
  "livekit.dll",
  "livekit_ffi.dll"
)
foreach ($file in $requiredFiles) {
  if (-not (Test-Path -LiteralPath (Join-Path $layout $file))) {
    throw "Required package payload is missing: $file"
  }
}

Add-Type -AssemblyName System.Drawing
$source = [System.Drawing.Image]::FromFile((Resolve-Path -LiteralPath $LogoPath).Path)
try {
  $assets = @(
    @{ Name = "StoreLogo.png"; Size = 50 },
    @{ Name = "Square44x44Logo.png"; Size = 44 },
    @{ Name = "Square150x150Logo.png"; Size = 150 }
  )

  foreach ($asset in $assets) {
    $size = [int]$asset.Size
    $bmp = New-Object System.Drawing.Bitmap $size, $size
    $graphics = [System.Drawing.Graphics]::FromImage($bmp)
    try {
      $graphics.Clear([System.Drawing.Color]::Transparent)
      $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
      $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
      $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
      $graphics.DrawImage($source, 0, 0, $size, $size)
      $bmp.Save(
        (Join-Path $layout ("Assets\" + $asset.Name)),
        [System.Drawing.Imaging.ImageFormat]::Png
      )
    } finally {
      $graphics.Dispose()
      $bmp.Dispose()
    }
  }
} finally {
  $source.Dispose()
}

function XmlEscape([string]$value) {
  return [System.Security.SecurityElement]::Escape($value)
}

$manifest = Get-Content -LiteralPath $templatePath -Raw
$manifest = $manifest.Replace("@@IDENTITY_NAME@@", (XmlEscape $identityName))
$manifest = $manifest.Replace("@@PUBLISHER@@", (XmlEscape $publisher))
$manifest = $manifest.Replace("@@PUBLISHER_DISPLAY_NAME@@", (XmlEscape $publisherDisplayName))
$manifest = $manifest.Replace("@@VERSION@@", (XmlEscape $packageVersion))
Set-Content -LiteralPath (Join-Path $layout "AppxManifest.xml") -Value $manifest -Encoding UTF8

$programFilesX86 = [Environment]::GetFolderPath([Environment+SpecialFolder]::ProgramFilesX86)
$sdkBin = Join-Path $programFilesX86 "Windows Kits\10\bin"
$makeAppxCandidates = Get-ChildItem -LiteralPath $sdkBin -Directory -ErrorAction SilentlyContinue |
  Where-Object { $_.Name -match '^10\.0\.[0-9]+\.[0-9]+$' } |
  Sort-Object { [version]$_.Name } -Descending |
  ForEach-Object { Join-Path $_.FullName "x64\makeappx.exe" } |
  Where-Object { Test-Path -LiteralPath $_ }

$makeAppx = $makeAppxCandidates | Select-Object -First 1
if (-not $makeAppx) {
  throw "makeappx.exe was not found in the installed Windows SDK."
}

$outputMsix = Join-Path $outputDirResolved "CommunityClashStreamer-$packageVersion-x64.msix"
if (Test-Path -LiteralPath $outputMsix) {
  Remove-Item -LiteralPath $outputMsix -Force
}

& $makeAppx pack /d $layout /p $outputMsix /o
if ($LASTEXITCODE -ne 0) {
  throw "MakeAppx failed with exit code $LASTEXITCODE"
}

$info = [ordered]@{
  storeReady = $storeReady
  identityName = $identityName
  publisher = $publisher
  publisherDisplayName = $publisherDisplayName
  packageVersion = $packageVersion
  architecture = "x64"
  msix = (Split-Path $outputMsix -Leaf)
}
$info | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $outputDirResolved "msix-build-info.json") -Encoding UTF8

if ($storeReady) {
  "CC_MSIX_ARTIFACT_NAME=CommunityClashStreamer-MSIX-store" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append
} else {
  "CC_MSIX_ARTIFACT_NAME=CommunityClashStreamer-MSIX-dev-validation" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append
}

Write-Host "MSIX created: $outputMsix"
Write-Host "Store-ready identity: $storeReady"

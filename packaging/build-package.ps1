[CmdletBinding()]
param(
    [string]$BaseUrl = 'http://49.12.198.91/sbstandalonebase',
    [string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot -Parent
$Version = '0.2.6'
$ModuleSize = 199119
$ModuleSha256 = '63652ce67df06a78abb84a4986253bdab02fbd7b7c000779c60b3d393ba9566b'
$ImageSize = 1806336
$ImageSha256 = '34f26e33d80393c1f1f497e85dff6b78dbc6fc0e2df98ed1150ea45c6631065d'
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $Root 'dist' }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$ZipPath = Join-Path $OutputDirectory "StandaloneBase-$Version.zip"
$Files = [ordered]@{
    'StandaloneBaseMeta.lua'   = (Join-Path $Root 'applet/StandaloneBaseMeta.lua')
    'StandaloneBaseApplet.lua' = (Join-Path $Root 'applet/StandaloneBaseApplet.lua')
    'strings.txt'               = (Join-Path $Root 'applet/strings.txt')
    'StorageManager.lua'        = (Join-Path $Root 'applet/StorageManager.lua')
    'ExtendedStorageState.lua'  = (Join-Path $Root 'applet/ExtendedStorageState.lua')
    'TimeSync.lua'              = (Join-Path $Root 'applet/TimeSync.lua')
    'TimeResolver.lua'          = (Join-Path $Root 'applet/TimeResolver.lua')
    'sbbase'                    = (Join-Path $Root 'build-arm/sbbase')
    'sbwebserver'               = (Join-Path $Root 'build-arm/sbwebserver')
    'sbproxy'                   = (Join-Path $Root 'build-arm/sbproxy')
    'sb-storage-helper'         = (Join-Path $Root 'build-arm/sb-storage-helper')
    'sb-storage-updater'        = (Join-Path $Root 'build-arm/sb-storage-updater')
    'storage-setup.sh'          = (Join-Path $Root 'scripts/storage-setup.sh')
    'storage-boot.sh'           = (Join-Path $Root 'scripts/storage-boot.sh')
    'sbubifs-authorized.ko'     = (Join-Path $Root 'artifacts/sbubifs-authorized.ko')
    'sbdata-empty.ubifs'        = (Join-Path $Root 'artifacts/ubi/sbdata-empty.ubifs')
    'index.html'                = (Join-Path $Root 'native/sbwebserver/web/index.html')
    'style.css'                 = (Join-Path $Root 'native/sbwebserver/web/css/style.css')
    'app.js'                    = (Join-Path $Root 'native/sbwebserver/web/js/app.js')
    'config.json'               = (Join-Path $Root 'config.json')
    'catalog.json'              = (Join-Path $Root 'config/catalog.example.json')
    'cacert.pem'                = (Join-Path $Root 'native/sbproxy/cacert.pem')
}
foreach ($Pair in $Files.GetEnumerator()) {
    if (-not (Test-Path -LiteralPath $Pair.Value -PathType Leaf)) { throw "Missing package input: $($Pair.Value)" }
}
$ModulePath = $Files['sbubifs-authorized.ko']
if ((Get-Item -LiteralPath $ModulePath).Length -ne $ModuleSize) {
    throw "Authorized SBUBIFS module has incorrect size"
}
if ((Get-FileHash -LiteralPath $ModulePath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $ModuleSha256) {
    throw "Authorized SBUBIFS module has incorrect SHA-256"
}
$ImagePath = $Files['sbdata-empty.ubifs']
if ((Get-Item -LiteralPath $ImagePath).Length -ne $ImageSize) {
    throw "SBData initialization image has incorrect size"
}
if ((Get-FileHash -LiteralPath $ImagePath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $ImageSha256) {
    throw "SBData initialization image has incorrect SHA-256"
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
Remove-Item -LiteralPath $ZipPath -Force -ErrorAction SilentlyContinue
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$Archive = [IO.Compression.ZipFile]::Open($ZipPath, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($Pair in $Files.GetEnumerator()) {
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($Archive, $Pair.Value, $Pair.Key,
            [IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
} finally { $Archive.Dispose() }
$Archive = [IO.Compression.ZipFile]::OpenRead($ZipPath)
try {
    $AllEntries = @($Archive.Entries | ForEach-Object FullName)
    $Entries = @($AllEntries | Where-Object { -not $_.EndsWith('/') })
}
finally { $Archive.Dispose() }
$Expected = @($Files.Keys)
$Missing = @($Expected | Where-Object { $_ -notin $Entries })
$Unexpected = @($Entries | Where-Object { $_ -notin $Expected })
$BadSeparators = @($Entries | Where-Object { $_.Contains([char]92) })
$NestedEntries = @($AllEntries | Where-Object { $_.Contains('/') })
if ($Missing.Count -or $Unexpected.Count -or $BadSeparators.Count -or $NestedEntries.Count) {
    throw "Invalid ZIP layout: $($AllEntries -join ', ')"
}
$Sha1 = (Get-FileHash -LiteralPath $ZipPath -Algorithm SHA1).Hash.ToLowerInvariant()
$Sha256 = (Get-FileHash -LiteralPath $ZipPath -Algorithm SHA256).Hash.ToLowerInvariant()
$Url = $BaseUrl.TrimEnd('/') + '/' + [IO.Path]::GetFileName($ZipPath)
$Xml = @"
<?xml version="1.0" encoding="UTF-8"?>
<extensions><details><title lang="EN">StandaloneBase Applet Repository</title></details><applets>
<applet name="StandaloneBase" version="$Version" target="baby" minTarget="7.7.3" maxTarget="*"><title lang="EN">Standalone Base</title><desc lang="EN">Local LMS-compatible infrastructure services for Squeezebox Radio.</desc><changes lang="EN">Add strictly gated Extended Storage creation for the validated virgin 7.7.3 layout.</changes><creator>Sjoerd Brandsma</creator><url>$Url</url><sha>$Sha1</sha></applet>
</applets></extensions>
"@
$Utf8 = New-Object Text.UTF8Encoding($false)
$XmlPath = Join-Path $OutputDirectory 'extensions.xml'
[IO.File]::WriteAllText($XmlPath, $Xml, $Utf8)
[xml]$Parsed = $Xml
if ($Parsed.extensions.applets.applet.sha -ne $Sha1) { throw 'Generated metadata validation failed' }
Write-Host "ZIP: $ZipPath"
Write-Host "ZIP size: $((Get-Item -LiteralPath $ZipPath).Length) bytes"
Write-Host "SHA-1: $Sha1"
Write-Host "SHA-256: $Sha256"
Write-Host "Entries: $($Entries -join ', ')"

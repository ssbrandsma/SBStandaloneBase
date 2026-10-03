[CmdletBinding()]
param([string]$Image = 'standalonebase-ubifs-tools:20090605')
$ErrorActionPreference = 'Stop'
$Root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$Out = Join-Path $Root 'artifacts/ubi'
docker build --pull --tag $Image $PSScriptRoot
New-Item -ItemType Directory -Force $Out | Out-Null
$id = docker create $Image
try { docker cp "${id}:/build/mkfs/mkfs.ubifs" (Join-Path $Out 'mkfs.ubifs-20090605-x86_64') }
finally { docker rm $id | Out-Null }
Get-FileHash (Join-Path $Out 'mkfs.ubifs-20090605-x86_64') -Algorithm SHA256

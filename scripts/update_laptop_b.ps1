# scripts/update_laptop_b.ps1
# Runs on Laptop B to download the latest precompiled release from GitHub and start the worker daemon.
param(
    [string]$InstallDir = "$env:USERPROFILE\DistributedGPU_Worker",
    [switch]$StartWorker
)

$ErrorActionPreference = "Stop"
$repoOwner = "CAsishSrinivas"
$repoName  = "DistributedGPU"

Write-Host "======================================================" -ForegroundColor Cyan
Write-Host " Fetching Latest DistributedGPU Release from GitHub" -ForegroundColor Cyan
Write-Host "======================================================" -ForegroundColor Cyan

# 1. Resolve Token from Git Credential Manager or Environment
$token = $env:GITHUB_TOKEN
if (-not $token) {
    try {
        $credInfo = echo "protocol=https`nhost=github.com" | git credential fill
        foreach ($line in ($credInfo -split "`n")) {
            if ($line.StartsWith("password=")) {
                $token = $line.Substring("password=".Length).Trim()
            }
        }
    } catch { }
}

$headers = @{
    "User-Agent" = "PowerShell"
    "Accept"     = "application/vnd.github.v3+json"
}
if ($token) {
    $headers["Authorization"] = "Bearer $token"
}

# 2. Query Latest Release
Write-Host "Querying latest release from GitHub..." -ForegroundColor Yellow
$release = Invoke-RestMethod -Uri "https://api.github.com/repos/$repoOwner/$repoName/releases/latest" -Headers $headers
Write-Host "Found Release: $($release.name) ($($release.tag_name))" -ForegroundColor Green

$asset = $release.assets | Where-Object { $_.name -like "*.zip" } | Select-Object -First 1
if (-not $asset) {
    throw "No zip binary asset found in latest release."
}

# 3. Download Release Asset
$zipDest = "$env:TEMP\$($asset.name)"
Write-Host "Downloading $($asset.name)..." -ForegroundColor Yellow

$assetHeaders = @{
    "User-Agent" = "PowerShell"
    "Accept"     = "application/octet-stream"
}
if ($token) {
    $assetHeaders["Authorization"] = "Bearer $token"
}

Invoke-RestMethod -Uri $asset.url -Headers $assetHeaders -OutFile $zipDest
Write-Host "Download complete: $zipDest" -ForegroundColor Green

# 4. Extract into Installation Directory
if (-not (Test-Path $InstallDir)) {
    New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null
}

Write-Host "Extracting to $InstallDir..." -ForegroundColor Yellow
Expand-Archive -Path $zipDest -DestinationPath $InstallDir -Force
Remove-Item $zipDest -Force

Write-Host "`n======================================================" -ForegroundColor Green
Write-Host " Installation complete at: $InstallDir" -ForegroundColor Green
Write-Host "======================================================" -ForegroundColor Green

# 5. Optionally launch worker daemon
if ($StartWorker -or (Test-Path "$InstallDir\bin\worker.exe")) {
    $workerPath = "$InstallDir\bin\worker.exe"
    Write-Host "`nStarting Worker daemon on port 7777..." -ForegroundColor Cyan
    Set-Location $InstallDir
    & $workerPath 7777
}

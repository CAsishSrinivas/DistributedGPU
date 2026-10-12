# scripts/publish_release.ps1
# Automates building, verifying tests, packaging, and publishing releases directly to GitHub Releases.
param(
    [string]$Version = ""
)

$ErrorActionPreference = "Stop"
$repoOwner = "CAsishSrinivas"
$repoName  = "DistributedGPU"

# 1. Determine Version Tag
if (-not $Version) {
    $dateTag = Get-Date -Format "yyyyMMdd.HHmm"
    $Version = "v$dateTag"
} elseif (-not $Version.StartsWith("v")) {
    $Version = "v$Version"
}

Write-Host "======================================================" -ForegroundColor Cyan
Write-Host " Publishing Release: $Version to GitHub" -ForegroundColor Cyan
Write-Host "======================================================" -ForegroundColor Cyan

# 2. Build Release Binaries
Write-Host "`n[1/5] Building Release binaries with CMake..." -ForegroundColor Yellow
cmake --build build --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw "Build failed!" }

# 3. Run CTest Verification
Write-Host "`n[2/5] Running CTest regression suite..." -ForegroundColor Yellow
ctest --test-dir build -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "Tests failed! Aborting release." }

# 4. Package Release Zip
Write-Host "`n[3/5] Packaging release binaries..." -ForegroundColor Yellow
$pkgDir = Join-Path $PSScriptRoot "staging"
if (Test-Path $pkgDir) { Remove-Item $pkgDir -Recurse -Force }
New-Item -ItemType Directory -Path "$pkgDir\bin" -Force | Out-Null
New-Item -ItemType Directory -Path "$pkgDir\shaders" -Force | Out-Null

Copy-Item "build\bin\Release\*.exe" "$pkgDir\bin\" -Force
Copy-Item "build\bin\Release\*.dll" "$pkgDir\bin\" -Force
Copy-Item "shaders\*" "$pkgDir\shaders\" -Force
Set-Content -Path "$pkgDir\dgpu.ini" -Value "192.168.1.14`n# writeback=1"

$zipName = "DistributedGPU-windows-x64-$Version.zip"
$zipPath = Join-Path $PSScriptRoot "..\$zipName"
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
Compress-Archive -Path "$pkgDir\*" -DestinationPath $zipPath -Force
Remove-Item $pkgDir -Recurse -Force

Write-Host "Package created: $zipName ($(([System.IO.FileInfo]$zipPath).Length) bytes)" -ForegroundColor Green

# 5. Resolve GitHub Credentials from Git Credential Helper
Write-Host "`n[4/5] Resolving GitHub credentials..." -ForegroundColor Yellow
$credInfo = echo "protocol=https`nhost=github.com" | git credential fill
$token = ""
foreach ($line in ($credInfo -split "`n")) {
    if ($line.StartsWith("password=")) {
        $token = $line.Substring("password=".Length).Trim()
    }
}
if (-not $token) {
    $token = $env:GITHUB_TOKEN
}
if (-not $token) {
    throw "GitHub token not found in Git Credential Manager or GITHUB_TOKEN environment variable."
}

# 6. Create Release on GitHub via REST API
Write-Host "`n[5/5] Creating GitHub Release & uploading asset..." -ForegroundColor Yellow
$headers = @{
    "Authorization" = "Bearer $token"
    "User-Agent"    = "PowerShell"
    "Accept"        = "application/vnd.github.v3+json"
}

$releaseBody = @{
    tag_name         = $Version
    target_commitish = "main"
    name             = "DistributedGPU $Version"
    body             = "Automated build release for Host & Worker. Includes precompiled binaries, dxgi.dll, shaders, and configs."
    draft            = $false
    prerelease       = $false
} | ConvertTo-Json

$release = Invoke-RestMethod -Uri "https://api.github.com/repos/$repoOwner/$repoName/releases" -Method Post -Headers $headers -Body $releaseBody

# Upload Binary Asset
$uploadUrl = $release.upload_url -replace '\{\?name,label\}', "?name=$zipName"
$zipBytes = [System.IO.File]::ReadAllBytes($zipPath)
$uploadHeaders = @{
    "Authorization" = "Bearer $token"
    "User-Agent"    = "PowerShell"
    "Content-Type"  = "application/zip"
}

$asset = Invoke-RestMethod -Uri $uploadUrl -Method Post -Headers $uploadHeaders -Body $zipBytes

Write-Host "`n======================================================" -ForegroundColor Green
Write-Host " Release successfully published!" -ForegroundColor Green
Write-Host " URL: $($release.html_url)" -ForegroundColor Cyan
Write-Host " Asset: $($asset.name)" -ForegroundColor Cyan
Write-Host "======================================================" -ForegroundColor Green

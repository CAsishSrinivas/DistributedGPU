# scripts/publish_release.ps1
# Automates building, verifying tests, packaging, and publishing releases directly to GitHub Releases.
# Supports branch-aware releases: 'develop-latest' (pre-release) and 'main-latest' (stable).
param(
    [string]$Branch = "",
    [string]$Version = ""
)

$ErrorActionPreference = "Stop"
$repoOwner = "CAsishSrinivas"
$repoName  = "DistributedGPU"

# 1. Determine Target Branch
if (-not $Branch) {
    try {
        $Branch = (git branch --show-current).Trim()
    } catch {
        $Branch = "develop"
    }
}
if (-not $Branch) { $Branch = "develop" }

# 2. Determine Tag, Title & Asset Name Based on Branch
$isPreRelease = ($Branch -eq "develop")
if ($Branch -eq "develop") {
    $tag      = "develop-latest"
    $title    = "DistributedGPU Develop (Latest Build)"
    $assetBase= "DistributedGPU-windows-x64-develop.zip"
    $desc     = "Continuous integration build from the 'develop' branch. Pre-release build for worker/testing nodes."
} else {
    $tag      = if ($Version) { "v$Version" } else { "main-latest" }
    $title    = "DistributedGPU Production (Stable)"
    $assetBase= "DistributedGPU-windows-x64-main.zip"
    $desc     = "Production release from the 'main' branch. Validated stable build."
}

Write-Host "======================================================" -ForegroundColor Cyan
Write-Host " Publishing Release for Branch: $Branch" -ForegroundColor Cyan
Write-Host " Tag: $tag | Asset: $assetBase" -ForegroundColor Cyan
Write-Host "======================================================" -ForegroundColor Cyan

# 3. Build Release Binaries
Write-Host "`n[1/5] Building Release binaries with CMake..." -ForegroundColor Yellow
cmake --build build --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw "Build failed!" }

# 4. Run CTest Verification
Write-Host "`n[2/5] Running CTest regression suite..." -ForegroundColor Yellow
ctest --test-dir build -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "Tests failed! Aborting release." }

# 5. Package Release Zip
Write-Host "`n[3/5] Packaging release binaries..." -ForegroundColor Yellow
$pkgDir = Join-Path $PSScriptRoot "staging"
if (Test-Path $pkgDir) { Remove-Item $pkgDir -Recurse -Force }
New-Item -ItemType Directory -Path "$pkgDir\bin" -Force | Out-Null
New-Item -ItemType Directory -Path "$pkgDir\shaders" -Force | Out-Null

Copy-Item "build\bin\Release\*.exe" "$pkgDir\bin\" -Force
Copy-Item "build\bin\Release\*.dll" "$pkgDir\bin\" -Force
Copy-Item "shaders\*" "$pkgDir\shaders\" -Force
Set-Content -Path "$pkgDir\dgpu.ini" -Value "192.168.1.14`n# writeback=1"

$zipPath = Join-Path $PSScriptRoot "..\$assetBase"
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
Compress-Archive -Path "$pkgDir\*" -DestinationPath $zipPath -Force
Remove-Item $pkgDir -Recurse -Force

Write-Host "Package created: $assetBase ($(([System.IO.FileInfo]$zipPath).Length) bytes)" -ForegroundColor Green

# 6. Resolve GitHub Credentials
Write-Host "`n[4/5] Resolving GitHub credentials..." -ForegroundColor Yellow
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
if (-not $token) {
    throw "GitHub token not found in Git Credential Manager or GITHUB_TOKEN environment variable."
}

$headers = @{
    "Authorization" = "Bearer $token"
    "User-Agent"    = "PowerShell"
    "Accept"        = "application/vnd.github.v3+json"
}

# 7. Delete Existing Release if Tag Exists to Ensure Fresh Release Asset
Write-Host "`n[5/5] Publishing Release to GitHub..." -ForegroundColor Yellow
try {
    $existing = Invoke-RestMethod -Uri "https://api.github.com/repos/$repoOwner/$repoName/releases/tags/$tag" -Headers $headers -ErrorAction SilentlyContinue
    if ($existing -and $existing.id) {
        Write-Host "Updating existing release for tag '$tag'..." -ForegroundColor Gray
        Invoke-RestMethod -Uri "https://api.github.com/repos/$repoOwner/$repoName/releases/$($existing.id)" -Method Delete -Headers $headers | Out-Null
        # Also delete old tag ref if needed
        try {
            Invoke-RestMethod -Uri "https://api.github.com/repos/$repoOwner/$repoName/git/refs/tags/$tag" -Method Delete -Headers $headers -ErrorAction SilentlyContinue | Out-Null
        } catch { }
    }
} catch { }

# Create New Release
$releaseBody = @{
    tag_name         = $tag
    target_commitish = $Branch
    name             = $title
    body             = "$desc`n`nGenerated on: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss UTC') from branch '$Branch'."
    draft            = $false
    prerelease       = $isPreRelease
} | ConvertTo-Json

$release = Invoke-RestMethod -Uri "https://api.github.com/repos/$repoOwner/$repoName/releases" -Method Post -Headers $headers -Body $releaseBody

# Upload Binary Asset
$uploadUrl = $release.upload_url -replace '\{\?name,label\}', "?name=$assetBase"
$zipBytes = [System.IO.File]::ReadAllBytes($zipPath)
$uploadHeaders = @{
    "Authorization" = "Bearer $token"
    "User-Agent"    = "PowerShell"
    "Content-Type"  = "application/zip"
}

$asset = Invoke-RestMethod -Uri $uploadUrl -Method Post -Headers $uploadHeaders -Body $zipBytes

Write-Host "`n======================================================" -ForegroundColor Green
Write-Host " Branch Release successfully published!" -ForegroundColor Green
Write-Host " Channel:  $Branch" -ForegroundColor Cyan
Write-Host " Release:  $($release.html_url)" -ForegroundColor Cyan
Write-Host " Asset:    $($asset.name) ($($asset.size) bytes)" -ForegroundColor Cyan
Write-Host "======================================================" -ForegroundColor Green

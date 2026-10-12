# scripts/generate_release_notes.ps1
# Generates structured, subsystem-categorized Markdown release notes
# based on git commit and file changes between two git revisions.
[CmdletBinding()]
param(
    [string]$TargetRef = "HEAD",
    [string]$BaseRef = "",
    [string]$Branch = "develop",
    [string]$OutputFile = ""
)

$ErrorActionPreference = "Stop"

# 1. Determine comparison range
if (-not $BaseRef) {
    # Check if HEAD has a parent
    $hasParent = git rev-parse --verify --quiet "$TargetRef~1"
    if ($hasParent) {
        $BaseRef = "$TargetRef~1"
    } else {
        $BaseRef = $TargetRef
    }
}

$range = if ($BaseRef -ne $TargetRef) { "$BaseRef..$TargetRef" } else { $TargetRef }

# 2. Extract commit log and subject
$commitLog = git log --oneline -n 10 $range
$latestSubject = (git log -n 1 --pretty=format:"%s" $TargetRef).Trim()

# Extract linked PR if available
$prMatch = [regex]::Match($latestSubject, '#(\d+)')
$prNumber = if ($prMatch.Success) { $prMatch.Groups[1].Value } else { "" }

# 3. Extract changed files and status
$diffFiles = @()
if ($BaseRef -ne $TargetRef) {
    $rawStatus = git diff --name-status $range
    foreach ($line in ($rawStatus -split "`r?`n")) {
        if (-not [string]::IsNullOrWhiteSpace($line)) {
            $parts = $line -split "`t"
            if ($parts.Count -ge 2) {
                $statusChar = $parts[0].Trim().Substring(0, 1)
                $filePath = $parts[1].Trim()
                $statusText = switch ($statusChar) {
                    "A" { "Added" }
                    "M" { "Modified" }
                    "D" { "Deleted" }
                    "R" { "Renamed" }
                    default { "Modified" }
                }
                $diffFiles += [PSCustomObject]@{
                    Status = $statusText
                    Path   = $filePath
                }
            }
        }
    }
}

# 4. Categorize files by subsystem
$subsystems = [ordered]@{
    "Host Runtime & Schedulers"    = @()
    "GPU Execution & Shaders"       = @()
    "Protocol & Networking"         = @()
    "Game Adapter & Injection"      = @()
    "Test Suites & Verification"    = @()
    "CI/CD, Workflows & Scripts"    = @()
    "Documentation & Repository"    = @()
}

foreach ($f in $diffFiles) {
    $p = $f.Path
    if ($p -like "apps/host/*" -or $p -like "libs/task_runtime/*") {
        $subsystems["Host Runtime & Schedulers"] += $f
    } elseif ($p -like "shaders/*" -or $p -like "libs/d3d12/*" -or $p -like "apps/worker/*") {
        $subsystems["GPU Execution & Shaders"] += $f
    } elseif ($p -like "libs/protocol/*" -or $p -like "libs/networking/*") {
        $subsystems["Protocol & Networking"] += $f
    } elseif ($p -like "apps/game_adapter/*") {
        $subsystems["Game Adapter & Injection"] += $f
    } elseif ($p -like "tests/*" -or $p -like "apps/benchmark/*") {
        $subsystems["Test Suites & Verification"] += $f
    } elseif ($p -like ".github/*" -or $p -like "scripts/*" -or $p -eq "CMakeLists.txt") {
        $subsystems["CI/CD, Workflows & Scripts"] += $f
    } else {
        $subsystems["Documentation & Repository"] += $f
    }
}

# 5. Extract diff stat summary
$diffStat = ""
if ($BaseRef -ne $TargetRef) {
    $diffStat = (git diff --shortstat $range).Trim()
}

# 6. Build Markdown Document
$md = [System.Text.StringBuilder]::new()

$tick = [char]96
$channelName = if ($Branch -eq "main") { "Production (Stable)" } else { "Develop (Pre-release)" }
$null = $md.AppendLine("## DistributedGPU $channelName Release")
$null = $md.AppendLine()

if ($prNumber) {
    $null = $md.AppendLine("> **Source PR**: [#$prNumber](https://github.com/CAsishSrinivas/DistributedGPU/pull/$prNumber)")
}
$null = $md.AppendLine("> **Commit**: $tick$latestSubject$tick")
if ($diffStat) {
    $null = $md.AppendLine("> **Changes**: $diffStat")
}
$null = $md.AppendLine()

# Subsystem changes
$null = $md.AppendLine("### 📦 Subsystem Modifications")
$null = $md.AppendLine()

$hasAnyCategorized = $false
foreach ($key in $subsystems.Keys) {
    $files = $subsystems[$key]
    if ($files.Count -gt 0) {
        $hasAnyCategorized = $true
        $icon = switch ($key) {
            "Host Runtime & Schedulers"    { "⚙️" }
            "GPU Execution & Shaders"       { "🖥️" }
            "Protocol & Networking"         { "🌐" }
            "Game Adapter & Injection"      { "🎮" }
            "Test Suites & Verification"    { "🧪" }
            "CI/CD, Workflows & Scripts"    { "🔧" }
            default                         { "📄" }
        }
        $null = $md.AppendLine("#### $icon $key")
        $null = $md.AppendLine()
        foreach ($f in $files) {
            $null = $md.AppendLine("- $tick$($f.Path)$tick *($($f.Status))*")
        }
        $null = $md.AppendLine()
    }
}

if (-not $hasAnyCategorized) {
    $null = $md.AppendLine("- *No modified source files recorded in diff range.*")
    $null = $md.AppendLine()
}

# Recent Commits Section
$null = $md.AppendLine("### 📜 Commit History in this Release")
$null = $md.AppendLine()
foreach ($line in ($commitLog -split "`r?`n")) {
    if (-not [string]::IsNullOrWhiteSpace($line)) {
        $null = $md.AppendLine("- $line")
    }
}
$null = $md.AppendLine()

# Cross-Device Deployment Instructions
$null = $md.AppendLine("### 💻 Cross-Device Update Instructions")
$null = $md.AppendLine()
$null = $md.AppendLine("To update Laptop B / worker nodes to this build:")
$null = $md.AppendLine('```powershell')
$null = $md.AppendLine('.\scripts\update_laptop_b.ps1 -StartWorker')
$null = $md.AppendLine('```')
$null = $md.AppendLine()

$releaseNotes = $md.ToString()

if ($OutputFile) {
    Set-Content -Path $OutputFile -Value $releaseNotes -Encoding UTF8
}

Write-Output $releaseNotes

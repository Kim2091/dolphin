<#
.SYNOPSIS
    Assemble a Dolphin + RTX Remix portable release from an existing build.

.DESCRIPTION
    Combines three inputs into one staged, scrubbed, versioned tree and zips it:

      1. The Dolphin build output   (build\release\x64\Binaries)
      2. The Remix runtime drop     (..\dolphin-remix-runtime)
      3. Docs and skeleton          (Tools\PortablePackage\docs, ...\skeleton)

    This script does NOT build anything. Build Dolphin and the runtime by the
    usual means first; this turns what you already have into a release.

    What ships is decided entirely by package.manifest.psd1. The staged tree is
    then re-checked against that manifest's Verify block before anything is
    compressed, because the exclusion rules are a blocklist and a blocklist only
    knows about the mistakes that have already been made once.

.PARAMETER Version
    Release version, e.g. "0.0.7" or "v0.0.7". Defaults to the newest version
    heading in docs\CHANGELOG.md.

.PARAMETER BinariesDir
    Dolphin build output. Defaults to <repo>\build\release\x64\Binaries.

.PARAMETER RuntimeDir
    Remix runtime drop. Defaults to <repo>\..\dolphin-remix-runtime.

.PARAMETER RuntimeRepo
    Optional path to the dxvk-remix checkout the runtime was built from. When
    given, its git provenance is recorded in VERSION.txt alongside the version
    string read out of the DLL itself.

.PARAMETER OutDir
    Where the staged folder and .zip are written. Defaults to <repo>\..\_releases.

.PARAMETER RootFolder
    Wrap the zip contents in a single top-level folder. Off by default, matching
    the flat layout of releases v0.0.3 through v0.0.6.

.PARAMETER NoZip
    Stage and verify, but stop before compressing. Useful for eyeballing a tree
    or for a dry run against a fresh build.

.PARAMETER AllowLargeFiles
    Permit files above the manifest's MaxFileSizeMB. Only the runtime DLL is
    expected to be that big; anything else is a symbol file or a stray backup.

.PARAMETER Force
    Overwrite an existing staged folder or zip for this version.

.EXAMPLE
    .\Tools\PortablePackage\Package-Portable.ps1
    Package the version at the top of the changelog, using default paths.

.EXAMPLE
    .\Tools\PortablePackage\Package-Portable.ps1 -Version 0.0.7 -NoZip
    Stage and verify 0.0.7 without spending three minutes on compression.
#>

[CmdletBinding()]
param(
    [string] $Version,
    [string] $BinariesDir,
    [string] $RuntimeDir,
    [string] $RuntimeRepo,
    [string] $OutDir,
    [switch] $RootFolder,
    [switch] $NoZip,
    [switch] $AllowLargeFiles,
    [switch] $Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# Console output
# ---------------------------------------------------------------------------

$script:StepNumber = 0

function Write-Step {
    param([string] $Message)
    $script:StepNumber++
    Write-Host ''
    Write-Host ("[{0}] {1}" -f $script:StepNumber, $Message) -ForegroundColor Cyan
}

function Write-Detail {
    param([string] $Message)
    Write-Host ("      " + $Message) -ForegroundColor DarkGray
}

function Write-Ok {
    param([string] $Message)
    Write-Host ("      " + $Message) -ForegroundColor Green
}

function Write-Warn {
    param([string] $Message)
    Write-Host ("      warning: " + $Message) -ForegroundColor Yellow
}

function Stop-WithError {
    param([string] $Message, [string[]] $Details = @())
    Write-Host ''
    Write-Host ("ERROR: " + $Message) -ForegroundColor Red
    foreach ($d in $Details) { Write-Host ("       " + $d) -ForegroundColor Red }
    Write-Host ''
    exit 1
}

function Format-Size {
    param([long] $Bytes)
    if ($Bytes -ge 1GB) { return ('{0:N2} GB' -f ($Bytes / 1GB)) }
    if ($Bytes -ge 1MB) { return ('{0:N1} MB' -f ($Bytes / 1MB)) }
    if ($Bytes -ge 1KB) { return ('{0:N1} KB' -f ($Bytes / 1KB)) }
    return ('{0} B' -f $Bytes)
}

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

$ToolDir  = $PSScriptRoot
$RepoRoot = (Resolve-Path (Join-Path $ToolDir '..\..')).Path
$DocsDir  = Join-Path $ToolDir 'docs'
$SkelDir  = Join-Path $ToolDir 'skeleton'
$ManifestPath = Join-Path $ToolDir 'package.manifest.psd1'

if (-not $BinariesDir) { $BinariesDir = Join-Path $RepoRoot 'build\release\x64\Binaries' }
if (-not $RuntimeDir)  { $RuntimeDir  = Join-Path $RepoRoot '..\dolphin-remix-runtime' }
if (-not $OutDir)      { $OutDir      = Join-Path $RepoRoot '..\_releases' }

Write-Host ''
Write-Host 'Dolphin + RTX Remix -- portable packager' -ForegroundColor White

# ---------------------------------------------------------------------------
# [1] Manifest and inputs
# ---------------------------------------------------------------------------

Write-Step 'Reading manifest and locating inputs'

if (-not (Test-Path $ManifestPath)) {
    Stop-WithError "Manifest not found at $ManifestPath"
}
$Manifest = Import-PowerShellDataFile -Path $ManifestPath

foreach ($pair in @(
        @{ Name = 'Dolphin build output'; Path = $BinariesDir },
        @{ Name = 'Remix runtime drop';   Path = $RuntimeDir },
        @{ Name = 'Docs';                 Path = $DocsDir },
        @{ Name = 'Skeleton';             Path = $SkelDir })) {
    if (-not (Test-Path $pair.Path)) {
        Stop-WithError ("{0} not found." -f $pair.Name) @(
            ("looked in: " + $pair.Path),
            'Pass -BinariesDir / -RuntimeDir explicitly if your layout differs.')
    }
}

$BinariesDir = (Resolve-Path $BinariesDir).Path
$RuntimeDir  = (Resolve-Path $RuntimeDir).Path

Write-Detail ("repo:      " + $RepoRoot)
Write-Detail ("binaries:  " + $BinariesDir)
Write-Detail ("runtime:   " + $RuntimeDir)

# Required inputs, named in the manifest so a layout change is a manifest edit.
$missing = @()
foreach ($r in $Manifest.Emulator.Required) {
    if (-not (Test-Path (Join-Path $BinariesDir $r))) { $missing += ("binaries\" + $r) }
}
foreach ($r in $Manifest.Runtime.Required) {
    if (-not (Test-Path (Join-Path $RuntimeDir $r))) { $missing += ("runtime\" + $r) }
}
if ($missing.Count -gt 0) {
    Stop-WithError 'Inputs are incomplete -- this build is not ready to package.' $missing
}
Write-Ok ("all {0} required inputs present" -f ($Manifest.Emulator.Required.Count + $Manifest.Runtime.Required.Count))

# ---------------------------------------------------------------------------
# [2] Version
# ---------------------------------------------------------------------------

Write-Step 'Determining version'

$ChangelogPath = Join-Path $DocsDir 'CHANGELOG.md'
$ChangelogDate = $null

# Read as UTF-8 explicitly. Select-String would decode the em dash in
# "## v0.0.7 <em-dash> 2026-08-31" as ANSI, and the date pattern below would then never
# match its own separator -- silently falling back to today's date, which is
# right on release day and wrong every other day.
$ChangelogText = ''
if (Test-Path $ChangelogPath) {
    $ChangelogText = [System.IO.File]::ReadAllText($ChangelogPath, [System.Text.Encoding]::UTF8)
}

if (-not $Version) {
    if (-not $ChangelogText) {
        Stop-WithError 'No -Version given and docs\CHANGELOG.md is missing.'
    }
    $head = [regex]::Match($ChangelogText, '(?m)^##\s+v([0-9]+\.[0-9]+\.[0-9]+)')
    if (-not $head.Success) {
        Stop-WithError 'No -Version given and no "## vX.Y.Z" heading found in docs\CHANGELOG.md.'
    }
    $Version = $head.Groups[1].Value
    Write-Detail ("taken from CHANGELOG.md: v" + $Version)
}

$Version = $Version.TrimStart('v', 'V')
if ($Version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') {
    Stop-WithError ("Version '{0}' is not X.Y.Z." -f $Version)
}
$VersionTag = 'v' + $Version

# Prefer the date the changelog gives this version; a release note that says
# one date and a VERSION.txt that says another is a small but real confusion.
if ($ChangelogText) {
    # Any dash-like separator: ASCII hyphen, en dash (U+2013), em dash (U+2014).
    # Written as escapes, not literal characters: PowerShell 5.1 parses a
    # BOM-less .ps1 as ANSI, so a literal em dash in this source would itself
    # arrive mangled and the class would never match the one in the changelog.
    $dateMatch = [regex]::Match($ChangelogText,
        '(?m)^##\s+v' + [regex]::Escape($Version) + '\s*[-–—]+\s*([0-9]{4}-[0-9]{2}-[0-9]{2})')
    if ($dateMatch.Success) { $ChangelogDate = $dateMatch.Groups[1].Value }
}
if ($ChangelogDate) {
    $BuildDate = $ChangelogDate
    Write-Detail ("date from changelog entry: " + $BuildDate)
} else {
    $BuildDate = (Get-Date -Format 'yyyy-MM-dd')
    Write-Warn ("no dated changelog entry for {0}; using today ({1})" -f $VersionTag, $BuildDate)
}

$PackageName = 'Dolphin-RTX-Remix-portable-' + $VersionTag
Write-Ok ("packaging " + $PackageName)

# ---------------------------------------------------------------------------
# [3] Provenance
# ---------------------------------------------------------------------------

Write-Step 'Collecting provenance'

# Run git and return its stdout lines, swallowing stderr.
#
# Necessary because of two PowerShell 5.1 behaviours that combine badly: stderr
# from a native command is wrapped into ErrorRecords, and $ErrorActionPreference
# 'Stop' then throws on them. git writes routine advice to stderr -- notably the
# "LF will be replaced by CRLF" notice on this repo -- so an ordinary `git diff`
# would abort the whole script.
function Invoke-Git {
    param([Parameter(ValueFromRemainingArguments = $true)] [string[]] $Arguments)

    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        # safecrlf=false silences the line-ending advice at its source.
        $out = & git -c core.safecrlf=false @Arguments 2>$null
        if ($null -eq $out) { return @() }
        return @($out)
    } catch {
        return @()
    } finally {
        $ErrorActionPreference = $prev
    }
}

function Get-GitInfo {
    param([string] $RepoPath, [string[]] $PreferredRemotes = @('origin'))

    $result = [ordered]@{
        Repo        = 'unknown'
        Remote      = 'unknown'
        Branch      = 'unknown'
        Commit      = 'unknown'
        CommitShort = 'unknown'
        Subject     = 'unknown'
        CommitDate  = [datetime]::MinValue
        Dirty       = $false
        Available   = $false
    }
    if (-not (Test-Path $RepoPath)) { return $result }

    $git = Get-Command git -ErrorAction SilentlyContinue
    if (-not $git) { return $result }

    Push-Location $RepoPath
    try {
        # @() at the call site: PowerShell unrolls a single-element array on
        # return, so a bare string comes back and StrictMode rejects .Count.
        $probe = @(Invoke-Git rev-parse --is-inside-work-tree)
        if ($probe.Count -eq 0) { return $result }

        $result.Commit      = (Invoke-Git rev-parse HEAD | Select-Object -First 1)
        $result.CommitShort = (Invoke-Git rev-parse --short HEAD | Select-Object -First 1)
        $result.Subject     = (Invoke-Git log -1 --format=%s | Select-Object -First 1)
        $result.Branch      = (Invoke-Git rev-parse --abbrev-ref HEAD | Select-Object -First 1)

        $iso = (Invoke-Git log -1 --format=%cI | Select-Object -First 1)
        if ($iso) {
            try { $result.CommitDate = [datetime]::Parse($iso) } catch { }
        }

        # Pick the remote that actually published these binaries. Taking
        # 'origin' unconditionally would name upstream dolphin-emu here and
        # point bug reporters at a project that never saw this code.
        foreach ($remote in $PreferredRemotes) {
            $url = (Invoke-Git config --get ("remote." + $remote + ".url") | Select-Object -First 1)
            if ($url) {
                # Normalise to a browsable https URL for the docs.
                $url = $url -replace '^git@github\.com:', 'https://github.com/'
                $url = $url -replace '\.git$', ''
                $result.Repo   = $url
                $result.Remote = $remote
                break
            }
        }

        $status = @(Invoke-Git status --porcelain)
        if ($status.Count -gt 0) { $result.Dirty = $true }

        $result.Available = $true
    } finally {
        Pop-Location
    }
    return $result
}

$preferredRemotes = @('origin')
if ($Manifest.ContainsKey('PreferredRemotes')) { $preferredRemotes = $Manifest.PreferredRemotes }

$DolphinGit = Get-GitInfo -RepoPath $RepoRoot -PreferredRemotes $preferredRemotes
if ($DolphinGit.Available) {
    Write-Detail ("emulator:  {0} @ {1}" -f $DolphinGit.Branch, $DolphinGit.CommitShort)
    Write-Detail ("           " + $DolphinGit.Subject)
    Write-Detail ("           {0}  (remote '{1}')" -f $DolphinGit.Repo, $DolphinGit.Remote)
    if ($DolphinGit.Dirty) {
        Write-Warn 'the Dolphin working tree has uncommitted changes -- the recorded commit will not fully describe these binaries'
    }
} else {
    Write-Warn 'no git provenance available for the Dolphin repo'
}

$RuntimeGit = $null
if ((-not $RuntimeRepo) -and $Manifest.ContainsKey('DefaultRuntimeRepo')) {
    $candidate = Join-Path $RepoRoot $Manifest.DefaultRuntimeRepo
    if (Test-Path $candidate) {
        $RuntimeRepo = (Resolve-Path $candidate).Path
        Write-Detail ("runtime repo (from manifest): " + $RuntimeRepo)
    }
}
if ($RuntimeRepo) {
    $RuntimeGit = Get-GitInfo -RepoPath $RuntimeRepo -PreferredRemotes $preferredRemotes
    if ($RuntimeGit.Available) {
        Write-Detail ("runtime:   {0} @ {1}" -f $RuntimeGit.Branch, $RuntimeGit.CommitShort)
    } else {
        Write-Warn ("-RuntimeRepo given but no git info found at " + $RuntimeRepo)
    }
}

# The runtime DLL carries its own identity. This is the authoritative check: a
# stock RTX Remix release will not match, and different forks have shipped the
# same ABI number from different lineages, so the ABI number proves nothing.
function Get-EmbeddedVersionString {
    param([string] $Path, [string] $Pattern)

    $regex     = [regex] $Pattern
    $chunkSize = 8MB
    $overlap   = 512
    $stream    = [System.IO.File]::OpenRead($Path)
    try {
        $buffer = New-Object byte[] $chunkSize
        $tail   = ''
        while ($true) {
            $read = $stream.Read($buffer, 0, $chunkSize)
            if ($read -le 0) { break }
            $text  = [System.Text.Encoding]::ASCII.GetString($buffer, 0, $read)
            $match = $regex.Match($tail + $text)
            if ($match.Success) { return $match.Value }
            if ($text.Length -ge $overlap) {
                $tail = $text.Substring($text.Length - $overlap)
            } else {
                $tail = $text
            }
        }
    } finally {
        $stream.Dispose()
    }
    return $null
}

$RuntimeDllPath = Join-Path $RuntimeDir $Manifest.Runtime.PrimaryDll
$RuntimeDllInfo = Get-Item $RuntimeDllPath

Write-Detail 'scanning the runtime DLL for its embedded version string...'
$RuntimeVersionString = Get-EmbeddedVersionString -Path $RuntimeDllPath -Pattern $Manifest.Runtime.VersionStringPattern
if (-not $RuntimeVersionString) {
    Stop-WithError 'The runtime DLL carries no recognisable fork version string.' @(
        ("file:    " + $RuntimeDllPath),
        ("pattern: " + $Manifest.Runtime.VersionStringPattern),
        '',
        'This is what a stock RTX Remix release looks like. Dolphin needs a build',
        'of the fork ABI line; a stock runtime fails at load with INCOMPATIBLE_VERSION.',
        'Check that the runtime drop holds the DLL you think it does.')
}
Write-Ok ("runtime identifies as " + $RuntimeVersionString)

# The version string ends in the commit the DLL was built from. If -RuntimeRepo
# points somewhere else, VERSION.txt would name a commit that never produced
# this binary -- which is worse than recording nothing, because it looks right.
if ($RuntimeGit -and $RuntimeGit.Available) {
    $embedded = $null
    if ($RuntimeVersionString -match '\+([0-9a-f]{7,})') { $embedded = $Matches[1] }

    if ($embedded) {
        $headFull = $RuntimeGit.Commit
        $n = [Math]::Min($embedded.Length, $headFull.Length)
        if ($headFull.Substring(0, $n) -ne $embedded.Substring(0, $n)) {
            Write-Warn ("-RuntimeRepo is at {0}, but the DLL was built from {1}" -f $RuntimeGit.CommitShort, $embedded)
            Write-Warn 'that checkout did not produce this runtime; its branch and commit will NOT be recorded'
            Write-Warn 'point -RuntimeRepo at the right checkout, or omit it and rely on the embedded string'
            $RuntimeGit = $null
        } else {
            Write-Ok ("runtime repo matches the DLL (" + $embedded + ")")
        }
    }
}

Write-Detail 'hashing the runtime DLL...'
$RuntimeHash = (Get-FileHash -Path $RuntimeDllPath -Algorithm SHA256).Hash.ToLower()
Write-Detail ("sha256:    " + $RuntimeHash)
Write-Detail ("size:      {0} bytes ({1})" -f $RuntimeDllInfo.Length, (Format-Size $RuntimeDllInfo.Length))

$DolphinExeInfo = Get-Item (Join-Path $BinariesDir 'Dolphin.exe')
Write-Detail ("Dolphin.exe built {0}" -f $DolphinExeInfo.LastWriteTime.ToString('yyyy-MM-dd HH:mm'))
Write-Detail ("runtime built     {0}" -f $RuntimeDllInfo.LastWriteTime.ToString('yyyy-MM-dd HH:mm'))

# Binaries older than the code they are about to be labelled with is the single
# easiest mistake to make here: edit, forget to build, package anyway, and ship
# a zip whose VERSION.txt describes code that is not in it.
#
# Two ways that happens, and both must be checked. Comparing against HEAD alone
# is not enough: a binary built AFTER the last commit still misses every
# uncommitted edit made since, and on this project the working tree is usually
# where the newest code lives.

# Dolphin bakes its git revision into the executable, so the binary can be asked
# directly which commit produced it. That beats every timestamp heuristic: file
# mtimes move when git rewrites line endings, which made an earlier version of
# this check cry stale over five files whose content had not changed at all.
if ($DolphinGit.Available -and $DolphinGit.Commit -ne 'unknown') {
    Write-Detail 'checking which commit Dolphin.exe was built from...'
    $builtFrom = Get-EmbeddedVersionString -Path $DolphinExeInfo.FullName `
                                           -Pattern ([regex]::Escape($DolphinGit.Commit))
    if ($builtFrom) {
        Write-Ok ("Dolphin.exe was built from HEAD (" + $DolphinGit.CommitShort + ")")
    } else {
        Write-Warn ("Dolphin.exe does not carry HEAD's revision (" + $DolphinGit.CommitShort + ")")
        Write-Warn 'these binaries are STALE -- rebuild Dolphin before releasing'
    }
}

# Uncommitted changes matter only where they change content. A file git has
# merely touched for line endings shows as modified but builds identically.
function Get-ChangedBuildInputs {
    param([string] $RepoPath)

    $git = Get-Command git -ErrorAction SilentlyContinue
    if (-not $git) { return @() }

    $buildInputs = @('.cpp', '.cc', '.cxx', '.c', '.h', '.hpp', '.inl', '.ui', '.qrc', '.rc', '.cmake', '.glsl')
    $changed = @()

    Push-Location $RepoPath
    try {
        # --numstat lists only files whose content actually differs.
        $lines  = @(Invoke-Git diff --numstat -- Source/)
        $lines += @(Invoke-Git diff --numstat --cached -- Source/)
        foreach ($line in $lines) {
            $parts = $line -split "`t"
            if ($parts.Count -lt 3) { continue }
            $path = $parts[2]
            $ext  = [System.IO.Path]::GetExtension($path).ToLower()
            if ($buildInputs -contains $ext) { $changed += $path }
        }

        # Untracked sources are content that has never been compiled.
        $untracked = @(Invoke-Git ls-files --others --exclude-standard -- Source/)
        foreach ($path in $untracked) {
            $ext = [System.IO.Path]::GetExtension($path).ToLower()
            if ($buildInputs -contains $ext) { $changed += $path }
        }
    } finally {
        Pop-Location
    }
    return @($changed | Sort-Object -Unique)
}

$changedInputs = @(Get-ChangedBuildInputs -RepoPath $RepoRoot)
if ($changedInputs.Count -gt 0) {
    Write-Warn ("{0} source file(s) have uncommitted changes not in these binaries:" -f $changedInputs.Count)
    foreach ($c in ($changedInputs | Select-Object -First 6)) { Write-Warn ("  " + $c) }
    if ($changedInputs.Count -gt 6) { Write-Warn ("  ... and {0} more" -f ($changedInputs.Count - 6)) }
} else {
    Write-Ok 'no uncommitted source changes'
}

# ---------------------------------------------------------------------------
# [4] Staging
# ---------------------------------------------------------------------------

Write-Step 'Staging'

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }
$OutDir = (Resolve-Path $OutDir).Path

$StageRoot = Join-Path $OutDir $PackageName
if (Test-Path $StageRoot) {
    if (-not $Force) {
        Stop-WithError ("Staging folder already exists: " + $StageRoot) @(
            'Pass -Force to replace it.')
    }
    Write-Detail 'removing previous staging folder'
    Remove-Item -Path $StageRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $StageRoot -Force | Out-Null
Write-Detail ("staging into " + $StageRoot)

function Test-NameExcluded {
    param([string] $Name, [string[]] $Patterns)
    foreach ($p in $Patterns) {
        if ($Name -like $p) { return $true }
    }
    return $false
}

function Copy-Tree {
    param(
        [string]   $Source,
        [string]   $Destination,
        [string[]] $ExcludeNames,
        [string[]] $ExcludeLeafNames,
        [string]   $Label
    )

    $copied  = 0
    $skipped = @()

    foreach ($entry in (Get-ChildItem -LiteralPath $Source -Force)) {
        if (Test-NameExcluded -Name $entry.Name -Patterns $ExcludeNames) {
            $skipped += $entry.Name
            continue
        }

        $target = Join-Path $Destination $entry.Name
        if ($entry.PSIsContainer) {
            Copy-Item -LiteralPath $entry.FullName -Destination $target -Recurse -Force
            $copied++
        } else {
            Copy-Item -LiteralPath $entry.FullName -Destination $target -Force
            $copied++
        }
    }

    # Second pass for leaf names that can hide at any depth.
    if ($ExcludeLeafNames -and $ExcludeLeafNames.Count -gt 0) {
        foreach ($pattern in $ExcludeLeafNames) {
            $hits = @(Get-ChildItem -LiteralPath $Destination -Recurse -Force -Filter $pattern -File -ErrorAction SilentlyContinue)
            foreach ($hit in $hits) {
                Remove-Item -LiteralPath $hit.FullName -Force
                $skipped += $hit.Name
            }
        }
    }

    Write-Detail ("{0}: {1} entries copied" -f $Label, $copied)
    if ($skipped.Count -gt 0) {
        Write-Detail ("{0}: {1} excluded -- {2}" -f $Label, $skipped.Count, (($skipped | Sort-Object -Unique) -join ', '))
    }
    return $copied
}

# 4a. Emulator build output.
$emuLeaf = @()
if ($Manifest.Emulator.ContainsKey('ExcludeLeafNames')) { $emuLeaf = $Manifest.Emulator.ExcludeLeafNames }
Copy-Tree -Source $BinariesDir -Destination $StageRoot `
          -ExcludeNames $Manifest.Emulator.ExcludeNames `
          -ExcludeLeafNames $emuLeaf `
          -Label 'emulator' | Out-Null

# 4b. Runtime drop, with the primary DLL renamed on the way in.
$rtLeaf = @()
if ($Manifest.Runtime.ContainsKey('ExcludeLeafNames')) { $rtLeaf = $Manifest.Runtime.ExcludeLeafNames }
$rtExclude = @($Manifest.Runtime.ExcludeNames) + @($Manifest.Runtime.PrimaryDll)
Copy-Tree -Source $RuntimeDir -Destination $StageRoot `
          -ExcludeNames $rtExclude `
          -ExcludeLeafNames $rtLeaf `
          -Label 'runtime' | Out-Null

Copy-Item -LiteralPath $RuntimeDllPath `
          -Destination (Join-Path $StageRoot $Manifest.Runtime.PrimaryDllAs) -Force
Write-Detail ("runtime: {0} -> {1}" -f $Manifest.Runtime.PrimaryDll, $Manifest.Runtime.PrimaryDllAs)

# 4c. Skeleton: portable marker, seeded User config, Games placeholder.
foreach ($entry in (Get-ChildItem -LiteralPath $SkelDir -Force)) {
    Copy-Item -LiteralPath $entry.FullName -Destination (Join-Path $StageRoot $entry.Name) -Recurse -Force
}
Write-Detail 'skeleton: portable.txt, User\, Games\'

# 4d. Static extras -- license texts for everything that is not Dolphin.
if ($Manifest.ContainsKey('ExtraFiles')) {
    foreach ($extra in $Manifest.ExtraFiles) {
        $src = Join-Path $ToolDir $extra.Source
        if (-not (Test-Path $src)) {
            Stop-WithError ("Extra file missing: " + $src)
        }
        $dst = Join-Path $StageRoot $extra.Dest
        $dstDir = Split-Path -Parent $dst
        if (-not (Test-Path $dstDir)) { New-Item -ItemType Directory -Path $dstDir -Force | Out-Null }
        Copy-Item -LiteralPath $src -Destination $dst -Force
    }
    Write-Detail ("extras: {0} license file(s)" -f $Manifest.ExtraFiles.Count)
}

# 4e. Directories that ship empty.
foreach ($d in $Manifest.EmptyDirs) {
    $p = Join-Path $StageRoot ($d -replace '/', '\')
    if (-not (Test-Path $p)) { New-Item -ItemType Directory -Path $p -Force | Out-Null }
}
Write-Detail ("empty dirs: {0} created" -f $Manifest.EmptyDirs.Count)

# ---------------------------------------------------------------------------
# [5] Documents
# ---------------------------------------------------------------------------

Write-Step 'Rendering documents'

$runtimeRepoUrl = 'https://github.com/Kim2091/dxvk-remix'
$runtimeBranch  = '(see version string)'
$runtimeCommit  = '(not recorded -- pass -RuntimeRepo)'
if ($RuntimeGit -and $RuntimeGit.Available) {
    if ($RuntimeGit.Repo -ne 'unknown') { $runtimeRepoUrl = $RuntimeGit.Repo }
    $runtimeBranch = $RuntimeGit.Branch
    $runtimeCommit = $RuntimeGit.Commit
}

$Tokens = [ordered]@{
    'VERSION'                = $VersionTag
    'VERSION_NUMBER'         = $Version
    'DATE'                   = $BuildDate
    'PACKAGE_NAME'           = $PackageName

    'DOLPHIN_REPO'           = $DolphinGit.Repo
    'DOLPHIN_BRANCH'         = $DolphinGit.Branch
    'DOLPHIN_COMMIT'         = $DolphinGit.Commit
    'DOLPHIN_COMMIT_SHORT'   = $DolphinGit.CommitShort
    'DOLPHIN_SUBJECT'        = $DolphinGit.Subject

    'RUNTIME_REPO'           = $runtimeRepoUrl
    'RUNTIME_BRANCH'         = $runtimeBranch
    'RUNTIME_COMMIT'         = $runtimeCommit
    'RUNTIME_VERSION_STRING' = $RuntimeVersionString
    'RUNTIME_DLL_NAME'       = $Manifest.Runtime.PrimaryDllAs
    'RUNTIME_SIZE'           = ('{0:N0}' -f $RuntimeDllInfo.Length)
    'RUNTIME_SHA256'         = $RuntimeHash
    'RUNTIME_SHA256_SHORT'   = $RuntimeHash.Substring(0, 16)
    'RUNTIME_BUILT'          = $RuntimeDllInfo.LastWriteTime.ToString('yyyy-MM-dd')
}

# Markers of text that has been through a wrong-codepage round trip. These
# documents are full of em dashes, and mangled ones are the kind of defect that
# reads as sloppiness to every user while being invisible to the person who
# shipped it.
$MojibakeMarkers = @(
    [char]0xFFFD                     # U+FFFD replacement character
    "$([char]0x00E2)$([char]0x20AC)" # 'a-circumflex' + euro: UTF-8 read as cp1252
    "$([char]0x00C3)$([char]0x00A9)" # 'A-tilde' + 'copyright': double-encoded
    "$([char]0x00EF)$([char]0x00BB)$([char]0x00BF)" # a BOM that became literal text
)

$unresolved = @()
$mangled    = @()
foreach ($doc in $Manifest.Documents) {
    $src = Join-Path $DocsDir $doc.Source
    if (-not (Test-Path $src)) {
        Stop-WithError ("Document source missing: " + $src)
    }
    # Read as UTF-8 explicitly. PS 5.1's Get-Content defaults to the system ANSI
    # codepage for a BOM-less file, which turns every em dash in these documents
    # into three characters of mojibake on the way through.
    $text = [System.IO.File]::ReadAllText($src, [System.Text.Encoding]::UTF8)

    foreach ($k in $Tokens.Keys) {
        $text = $text.Replace('{{' + $k + '}}', [string]$Tokens[$k])
    }

    # A placeholder that survives substitution would ship as literal braces.
    $left = [regex]::Matches($text, '\{\{[A-Z0-9_]+\}\}')
    foreach ($m in $left) { $unresolved += ("{0}: {1}" -f $doc.Source, $m.Value) }

    foreach ($marker in $MojibakeMarkers) {
        if ($text.Contains($marker)) {
            $mangled += ("{0}: contains mis-decoded text near '{1}'" -f $doc.Source, $marker)
        }
    }

    # UTF-8 without a BOM: Set-Content -Encoding UTF8 would add one on PS 5.1,
    # and a BOM in an .ini is the sort of thing that quietly breaks a parser.
    $dest = Join-Path $StageRoot $doc.Dest
    [System.IO.File]::WriteAllText($dest, $text, (New-Object System.Text.UTF8Encoding($false)))
    Write-Detail ("{0} -> {1}" -f $doc.Source, $doc.Dest)
}

if ($unresolved.Count -gt 0) {
    Stop-WithError 'Documents contain placeholders this script does not know how to fill.' `
        (($unresolved | Sort-Object -Unique) + @('', 'Add the token to $Tokens, or fix the typo in the document.'))
}

if ($mangled.Count -gt 0) {
    Stop-WithError 'Documents came out mis-encoded.' `
        (($mangled | Sort-Object -Unique) + @(
            '',
            'The sources in docs\ must be UTF-8. Check that whatever last edited',
            'them did not save as ANSI, and that this script still reads them as UTF-8.'))
}

Write-Ok ("{0} documents rendered at {1}, UTF-8 clean" -f $Manifest.Documents.Count, $VersionTag)

# ---------------------------------------------------------------------------
# [6] Verification
# ---------------------------------------------------------------------------

Write-Step 'Verifying the staged tree'

$problems = @()

foreach ($rel in $Manifest.Verify.MustExist) {
    $p = Join-Path $StageRoot ($rel -replace '/', '\')
    if (-not (Test-Path $p)) { $problems += ("missing: " + $rel) }
}

$allFiles = @(Get-ChildItem -LiteralPath $StageRoot -Recurse -Force -File)
foreach ($pattern in $Manifest.Verify.MustNotExist) {
    foreach ($f in $allFiles) {
        if ($f.Name -like $pattern) {
            $rel = $f.FullName.Substring($StageRoot.Length + 1)
            $problems += ("must not ship: {0}  (matched '{1}')" -f $rel, $pattern)
        }
    }
}

$maxBytes = $Manifest.Verify.MaxFileSizeMB * 1MB
foreach ($f in $allFiles) {
    if ($f.Length -le $maxBytes) { continue }
    $rel = $f.FullName.Substring($StageRoot.Length + 1)
    if ($rel -eq $Manifest.Runtime.PrimaryDllAs) { continue }
    if ($AllowLargeFiles) {
        Write-Warn ("{0} is {1} -- allowed by -AllowLargeFiles" -f $rel, (Format-Size $f.Length))
    } else {
        $problems += ("unexpectedly large ({0}): {1}" -f (Format-Size $f.Length), $rel)
    }
}

$totalBytes = ($allFiles | Measure-Object -Property Length -Sum).Sum
$totalMB    = [math]::Round($totalBytes / 1MB)
if ($totalMB -lt $Manifest.Verify.MinTotalSizeMB) {
    $problems += ("package is only {0} MB; expected at least {1} MB -- something did not get copied" -f $totalMB, $Manifest.Verify.MinTotalSizeMB)
}
if ($totalMB -gt $Manifest.Verify.MaxTotalSizeMB) {
    $problems += ("package is {0} MB; expected at most {1} MB -- something got copied twice" -f $totalMB, $Manifest.Verify.MaxTotalSizeMB)
}

if ($problems.Count -gt 0) {
    Stop-WithError ("Staged tree failed verification ({0} problems). Nothing was compressed." -f $problems.Count) `
        ($problems + @('', ("The staged tree is left at " + $StageRoot + " for inspection.")))
}

Write-Ok ("{0} files, {1}" -f $allFiles.Count, (Format-Size $totalBytes))
Write-Ok 'no forbidden files, all required paths present'

# ---------------------------------------------------------------------------
# [7] Compression
# ---------------------------------------------------------------------------

if ($NoZip) {
    Write-Step 'Skipping compression (-NoZip)'
    Write-Host ''
    Write-Host ('Staged: ' + $StageRoot) -ForegroundColor Green
    Write-Host ''
    exit 0
}

Write-Step 'Compressing'

$ZipPath = Join-Path $OutDir ($PackageName + '.zip')
if (Test-Path $ZipPath) {
    if (-not $Force) {
        Stop-WithError ("Zip already exists: " + $ZipPath) @('Pass -Force to replace it.')
    }
    Remove-Item -LiteralPath $ZipPath -Force
}

$sw = [System.Diagnostics.Stopwatch]::StartNew()
$sevenZip = Get-Command 7z -ErrorAction SilentlyContinue

if ($sevenZip) {
    Write-Detail 'using 7z (multithreaded)'
    if ($RootFolder) {
        # Archive the staging folder itself, so entries are prefixed with it.
        Push-Location $OutDir
        try {
            & $sevenZip.Source a -tzip -mx=5 -mmt=on -bso0 -bsp0 -- $ZipPath $PackageName | Out-Null
        } finally { Pop-Location }
    } else {
        Push-Location $StageRoot
        try {
            & $sevenZip.Source a -tzip -mx=5 -mmt=on -bso0 -bsp0 -- $ZipPath '*' | Out-Null
        } finally { Pop-Location }
    }
    if ($LASTEXITCODE -ne 0) {
        Stop-WithError ("7z exited with code " + $LASTEXITCODE)
    }
} else {
    Write-Detail 'using .NET ZipFile (7z not found; this is slower)'
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    if ($RootFolder) {
        [System.IO.Compression.ZipFile]::CreateFromDirectory(
            $StageRoot, $ZipPath, [System.IO.Compression.CompressionLevel]::Optimal, $true)
    } else {
        [System.IO.Compression.ZipFile]::CreateFromDirectory(
            $StageRoot, $ZipPath, [System.IO.Compression.CompressionLevel]::Optimal, $false)
    }
}

$sw.Stop()
$zipInfo = Get-Item $ZipPath
Write-Ok ("{0} in {1:N0}s ({2:N0}% of staged size)" -f `
    (Format-Size $zipInfo.Length), $sw.Elapsed.TotalSeconds, (100 * $zipInfo.Length / $totalBytes))

# ---------------------------------------------------------------------------
# [8] Release record
# ---------------------------------------------------------------------------

Write-Step 'Writing release record'

$zipHash = (Get-FileHash -Path $ZipPath -Algorithm SHA256).Hash.ToLower()

$record = @(
    ("Package:        " + $PackageName + '.zip'),
    ("Built:          " + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')),
    ("Size:           {0:N0} bytes ({1})" -f $zipInfo.Length, (Format-Size $zipInfo.Length)),
    ("SHA-256:        " + $zipHash),
    '',
    'Emulator',
    ("  Repository:   " + $DolphinGit.Repo),
    ("  Branch:       " + $DolphinGit.Branch),
    ("  Commit:       " + $DolphinGit.Commit),
    ("  Subject:      " + $DolphinGit.Subject),
    ("  Tree clean:   " + (-not $DolphinGit.Dirty)),
    '',
    'Remix runtime',
    ("  Version:      " + $RuntimeVersionString),
    ("  Shipped as:   " + $Manifest.Runtime.PrimaryDllAs),
    ("  Size:         {0:N0} bytes" -f $RuntimeDllInfo.Length),
    ("  SHA-256:      " + $RuntimeHash),
    ("  Source repo:  " + $runtimeRepoUrl),
    ("  Branch:       " + $runtimeBranch),
    ("  Commit:       " + $runtimeCommit),
    '',
    'Inputs',
    ("  Binaries:     " + $BinariesDir),
    ("  Runtime:      " + $RuntimeDir)
) -join "`r`n"

$recordPath = Join-Path $OutDir ($PackageName + '.release.txt')
Set-Content -LiteralPath $recordPath -Value $record -Encoding UTF8
Write-Detail ("release record -> " + (Split-Path -Leaf $recordPath))

# A checksum file next to the zip, in the format people expect to verify with.
$sumsPath = Join-Path $OutDir ($PackageName + '.zip.sha256')
Set-Content -LiteralPath $sumsPath -Value ("{0}  {1}.zip" -f $zipHash, $PackageName) -Encoding ASCII
Write-Detail ("checksum       -> " + (Split-Path -Leaf $sumsPath))

Write-Host ''
Write-Host '----------------------------------------------------------------' -ForegroundColor DarkGray
Write-Host ("  " + $PackageName + '.zip') -ForegroundColor Green
Write-Host ("  " + (Format-Size $zipInfo.Length) + '   sha256 ' + $zipHash.Substring(0, 16) + '...') -ForegroundColor Green
Write-Host ("  " + $OutDir) -ForegroundColor DarkGray
Write-Host '----------------------------------------------------------------' -ForegroundColor DarkGray
Write-Host ''
if ($DolphinGit.Dirty) {
    Write-Host 'Note: packaged from a dirty working tree. The commit in VERSION.txt' -ForegroundColor Yellow
    Write-Host '      does not fully describe what is in this zip.' -ForegroundColor Yellow
    Write-Host ''
}

# install.ps1 — One-line installer for codebase-memory-mcp (Windows).
#
# Usage: see README.md for install instructions.
#
# Environment:
#   CBM_DOWNLOAD_URL  Override base URL for downloads (for testing)

$ErrorActionPreference = "Stop"

# Enforce TLS 1.2+ (older PowerShell defaults to TLS 1.0 which GitHub rejects)
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 -bor [Net.SecurityProtocolType]::Tls13

$Repo = "0ctacity/codebase-memory-mcp"
$InstallDir = "$env:LOCALAPPDATA\Programs\codebase-memory-mcp"
$BinName = "codebase-memory-mcp.exe"
$BaseUrl = if ($env:CBM_DOWNLOAD_URL) { $env:CBM_DOWNLOAD_URL } else { "https://github.com/$Repo/releases/latest/download" }

# Security: reject non-HTTPS download URLs (defense-in-depth)
if (-not $BaseUrl.StartsWith("https://") -and -not $BaseUrl.StartsWith("http://localhost") -and -not $BaseUrl.StartsWith("http://127.0.0.1")) {
    Write-Host "error: refusing non-HTTPS download URL: $BaseUrl" -ForegroundColor Red
    exit 1
}

# Detect variant from args (--ui or --standard)
$Variant = "standard"
$SkipConfig = $false
$Replace = $false
$ReplaceConfig = $false
foreach ($arg in $args) {
    if ($arg -eq "--ui") { $Variant = "ui" }
    if ($arg -eq "--standard") { $Variant = "standard" }
    if ($arg -eq "--skip-config") { $SkipConfig = $true }
    if ($arg -eq "--replace") { $Replace = $true }
    if ($arg -eq "--replace-config") { $ReplaceConfig = $true }
    if ($arg -like "--dir=*") { $InstallDir = $arg.Substring(6) }
}

# Detect the OS architecture. RuntimeInformation.OSArchitecture reports the real
# OS arch (Arm64) even from an x64 process running under emulation on ARM64 --
# unlike $env:PROCESSOR_ARCHITECTURE, which reports the emulated "AMD64", and
# PROCESSOR_ARCHITEW6432, which is unset for 64-bit emulated processes. Fall back
# to the env vars only if the .NET API is somehow unavailable.
if ($env:CBM_ARCH) {
    # Explicit override wins — used by CI/tests, and an escape hatch under x64
    # emulation on ARM64 where no in-process detection is reliable.
    $Arch = $env:CBM_ARCH
} else {
    try {
        $osArch = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture
        $Arch = if ($osArch -eq 'Arm64') { "arm64" } else { "amd64" }
    } catch {
        if ($env:PROCESSOR_ARCHITECTURE -eq "ARM64" -or $env:PROCESSOR_ARCHITEW6432 -eq "ARM64") {
            $Arch = "arm64"
        } else {
            $Arch = "amd64"
        }
    }
}

Write-Host "codebase-memory-mcp installer (Windows)"
Write-Host "  variant: $Variant"
Write-Host "  arch:    $Arch"
Write-Host "  target:  $InstallDir\$BinName"
Write-Host ""

# Build download URL
if ($Variant -eq "ui") {
    $Archive = "codebase-memory-mcp-ui-windows-$Arch.zip"
} else {
    $Archive = "codebase-memory-mcp-windows-$Arch.zip"
}
$Url = "$BaseUrl/$Archive"

# Download
$TmpDir = Join-Path ([System.IO.Path]::GetTempPath()) "cbm-install-$(Get-Random)"
New-Item -ItemType Directory -Path $TmpDir -Force | Out-Null

Write-Host "Downloading $Archive..."
try {
    Invoke-WebRequest -Uri $Url -OutFile "$TmpDir\$Archive" -UseBasicParsing
} catch {
    Write-Host "error: download failed: $_" -ForegroundColor Red
    Remove-Item -Recurse -Force $TmpDir -ErrorAction SilentlyContinue
    exit 1
}


# Checksum verification
$ChecksumUrl = "$BaseUrl/checksums.txt"
try {
    Invoke-WebRequest -Uri $ChecksumUrl -OutFile "$TmpDir\checksums.txt" -UseBasicParsing
    $checksumLine = Get-Content "$TmpDir\checksums.txt" | Where-Object { $_ -like "*$Archive*" }
    if ($checksumLine) {
        $expected = ($checksumLine -split '\s+')[0]
        $actual = (Get-FileHash -Path "$TmpDir\$Archive" -Algorithm SHA256).Hash.ToLower()
        if ($expected -ne $actual) {
            Write-Host "error: CHECKSUM MISMATCH!" -ForegroundColor Red
            Write-Host "  expected: $expected"
            Write-Host "  actual:   $actual"
            Remove-Item -Recurse -Force $TmpDir
            exit 1
        }
        Write-Host "Checksum verified."
    }
} catch {
    Write-Host "warning: could not verify checksum (non-fatal)"
}

# Extract
Write-Host "Extracting..."
Expand-Archive -Path "$TmpDir\$Archive" -DestinationPath $TmpDir -Force

$DlBin = Join-Path $TmpDir $BinName
if (-not (Test-Path $DlBin)) {
    # UI variant may have different name in zip
    $UiBin = Join-Path $TmpDir "codebase-memory-mcp-ui.exe"
    if (Test-Path $UiBin) {
        Rename-Item $UiBin $BinName
        $DlBin = Join-Path $TmpDir $BinName
    } else {
        Write-Host "error: binary not found after extraction" -ForegroundColor Red
        Remove-Item -Recurse -Force $TmpDir
        exit 1
    }
}

# Install
New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null
$Dest = Join-Path $InstallDir $BinName

# Identical binaries are a true no-op. Refuse a different target unless the
# caller explicitly authorizes replacement.
if (Test-Path $Dest) {
    $CurrentHash = (Get-FileHash -Path $Dest -Algorithm SHA256).Hash
    $IncomingHash = (Get-FileHash -Path $DlBin -Algorithm SHA256).Hash
    if ($CurrentHash -eq $IncomingHash) {
        Write-Host "already installed: $Dest"
        Remove-Item -Recurse -Force $TmpDir -ErrorAction SilentlyContinue
        exit 0
    }
    if (-not $Replace) {
        Write-Host "error: a different binary already exists at $Dest" -ForegroundColor Red
        Write-Host "Re-run with --replace to replace it explicitly."
        Remove-Item -Recurse -Force $TmpDir -ErrorAction SilentlyContinue
        exit 1
    }
}

$InstallTmp = Join-Path $InstallDir ".codebase-memory-mcp.install.$([Guid]::NewGuid().ToString('N')).exe"
Copy-Item $DlBin $InstallTmp

# Verify before atomically moving the binary into place.
try {
    $ver = & $InstallTmp --version 2>&1
} catch {
    Write-Host "error: installed binary failed to run" -ForegroundColor Red
    Remove-Item $InstallTmp -Force -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force $TmpDir
    exit 1
}
if ((Test-Path $Dest) -and -not $Replace) {
    Write-Host "error: install target appeared while installing: $Dest" -ForegroundColor Red
    Remove-Item $InstallTmp -Force -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force $TmpDir
    exit 1
}
try {
    Move-Item $InstallTmp $Dest -Force
} catch {
    Remove-Item $InstallTmp -Force -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force $TmpDir -ErrorAction SilentlyContinue
    throw
}
Write-Host "Installed: $ver"

# Configure agents
if ($SkipConfig) {
    Write-Host ""
    Write-Host "Skipping agent configuration (--skip-config)"
} else {
    Write-Host ""
    Write-Host "Configuring coding agents..."
    $ConfigArgs = @("install", "-y")
    if ($ReplaceConfig) { $ConfigArgs += "--replace-config" }
    & $Dest @ConfigArgs 2>&1 | Write-Host
    if ($LASTEXITCODE -ne 0) {
        Write-Host ""
        Write-Host "error: agent configuration failed (exit code $LASTEXITCODE)" -ForegroundColor Red
        Write-Host "The binary was installed, but no coding agents were configured."
        Write-Host "Run manually to configure: `"$Dest`" install"
        exit 1
    }
}

# Add to PATH (user scope, no admin needed)
$UserPath = [Environment]::GetEnvironmentVariable("PATH", "User")
if ($UserPath -notlike "*$InstallDir*") {
    [Environment]::SetEnvironmentVariable("PATH", "$UserPath;$InstallDir", "User")
    $env:PATH = "$env:PATH;$InstallDir"
    Write-Host "Added $InstallDir to user PATH"
}

# Cleanup
Remove-Item -Recurse -Force $TmpDir -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "Done! Restart your terminal and coding agent to start using codebase-memory-mcp."

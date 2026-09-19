# computer-control installer for Windows.
#
#   irm https://raw.githubusercontent.com/minhnd410/computer-control/main/packaging/scripts/install.ps1 | iex
#
# Environment:
#   CC_PREFIX    install root, default $env:LOCALAPPDATA\Programs\computer-control
#   CC_VERSION   a tag such as v0.8.1, default the latest release

$ErrorActionPreference = 'Stop'

$repo    = 'minhnd410/computer-control'
$version = if ($env:CC_VERSION) { $env:CC_VERSION } else { 'latest' }
$prefix  = if ($env:CC_PREFIX)  { $env:CC_PREFIX  } else { "$env:LOCALAPPDATA\Programs\computer-control" }

function Fail($message) {
    Write-Host "error: $message" -ForegroundColor Red
    exit 1
}

$arch = (Get-CimInstance Win32_Processor).Architecture
if ($arch -ne 9) {
    # 9 is x64. There is no arm64 archive yet, and installing the x64 one
    # under emulation would work but inject input through a translation layer,
    # which is not something to do silently.
    Fail "only x64 is published. On arm64, build from source: https://github.com/$repo/blob/main/docs/install.md"
}

$archive = 'computer-control-windows-x86_64.zip'
$base = if ($version -eq 'latest') {
    "https://github.com/$repo/releases/latest/download"
} else {
    "https://github.com/$repo/releases/download/$version"
}

$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ([System.IO.Path]::GetRandomFileName())
New-Item -ItemType Directory -Path $tmp -Force | Out-Null

try {
    Write-Host "computer-control: windows-x86_64, installing to $prefix"

    try {
        Invoke-WebRequest -Uri "$base/$archive" -OutFile "$tmp\$archive" -UseBasicParsing
    } catch {
        Fail "could not download $base/$archive`n       Check https://github.com/$repo/releases for what is published."
    }

    # Every release publishes a .sha256 beside the archive. This binary can
    # drive the desktop; an unverified download is not worth the convenience.
    try {
        Invoke-WebRequest -Uri "$base/$archive.sha256" -OutFile "$tmp\$archive.sha256" -UseBasicParsing
    } catch {
        Fail "no checksum published for $archive; refusing to install unverified"
    }

    $expected = ((Get-Content "$tmp\$archive.sha256" -Raw).Trim() -split '\s+')[0]
    $actual   = (Get-FileHash "$tmp\$archive" -Algorithm SHA256).Hash
    if ($expected.ToLower() -ne $actual.ToLower()) {
        Fail "checksum mismatch for $archive`n       expected $expected`n       got      $actual`n       Do not use this download."
    }
    Write-Host '  checksum ok'

    Expand-Archive -Path "$tmp\$archive" -DestinationPath $tmp -Force
    $binary = Join-Path $tmp 'computer-control\computer-control-mcp.exe'
    if (-not (Test-Path $binary)) {
        Fail 'unexpected archive layout: no computer-control\computer-control-mcp.exe'
    }

    New-Item -ItemType Directory -Path $prefix -Force | Out-Null
    Copy-Item $binary (Join-Path $prefix 'computer-control-mcp.exe') -Force

    $installed = & (Join-Path $prefix 'computer-control-mcp.exe') --version | Select-Object -First 1
    Write-Host "  installed $installed"

    # Persist to the user PATH rather than the machine one, so this needs no
    # elevation and cannot affect other accounts.
    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    if ($userPath -notlike "*$prefix*") {
        [Environment]::SetEnvironmentVariable('Path', "$userPath;$prefix", 'User')
        Write-Host "  added $prefix to your PATH (restart the terminal to pick it up)"
    }

    Write-Host @"

Next:
    computer-control-mcp setup

That registers the server with the MCP clients on this machine. Windows
needs no permission grants; the one thing to know is that input aimed at a
window running elevated is silently discarded unless this runs elevated too.
"@
} finally {
    Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
}

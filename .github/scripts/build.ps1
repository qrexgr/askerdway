param([Parameter(Mandatory)][string]$SourceDir, [Parameter(Mandatory)][string]$OutputDir)
$ErrorActionPreference = 'Stop'
$sourceRoot = (Resolve-Path -LiteralPath $SourceDir).Path
$outputRoot = [IO.Path]::GetFullPath($OutputDir)
$buildRoot = Join-Path $sourceRoot 'build'
$sourceCommit = (git -C $sourceRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot resolve source commit' }

rustup toolchain install 1.95.0 --profile minimal
if ($LASTEXITCODE -ne 0) { throw 'Rust installation failed' }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsRoot = (& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
if ($LASTEXITCODE -ne 0 -or -not $vsRoot) { throw 'MSVC C++ tools were not found' }
& (Join-Path $vsRoot 'Common7/Tools/Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { throw 'MSVC environment initialization failed' }
cmake -S $sourceRoot -B $buildRoot -G Ninja -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed' }
cmake --build $buildRoot --config Release --target askerdway --parallel 2
if ($LASTEXITCODE -ne 0) { throw 'Release build failed' }
$dll = Join-Path $buildRoot 'askerdway.dll'

@'
import ctypes, pathlib, re, sys
path = pathlib.Path(sys.argv[1])
data = path.read_bytes()
for encoding in ('utf-8', 'utf-16le'):
    text = data.decode(encoding, errors='ignore')
    assert not re.search(r'[A-Za-z]:[/\\]+Users[/\\]|D:[/\\]a[/\\]', text, re.I), 'Unexpected build path in DLL'
library = ctypes.WinDLL(str(path))
library.AskerdwayApi_GetAbiVersion.restype = ctypes.c_uint32
assert library.AskerdwayApi_GetAbiVersion() == 0x10000, 'Unexpected ABI'
print('DLL load and ABI check passed')
'@ | python - $dll
if ($LASTEXITCODE -ne 0) { throw 'DLL check failed' }

New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
foreach ($name in @('LICENSE', 'THIRD_PARTY_NOTICES.txt')) {
    Copy-Item -LiteralPath (Join-Path $sourceRoot $name) -Destination (Join-Path $outputRoot $name)
}
Copy-Item -LiteralPath (Join-Path $sourceRoot 'assets/fonts/LICENSE-FONT.txt') -Destination (Join-Path $outputRoot 'LICENSE-FONT.txt')
Copy-Item -LiteralPath $dll -Destination (Join-Path $outputRoot 'askerdway.dll')
$info = [ordered]@{
    source_commit = $sourceCommit
    source_tree = (git -C $sourceRoot rev-parse 'HEAD^{tree}').Trim()
    workflow_commit = $env:WORKFLOW_COMMIT
    release_tag = $env:RELEASE_TAG
    run_url = $env:RUN_URL
    run_attempt = $env:GITHUB_RUN_ATTEMPT
    built_at_utc = [DateTime]::UtcNow.ToString('o')
    dll_sha256 = (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash.ToLowerInvariant()
    rust = (rustc +1.95.0 --version)
    cmake = (cmake --version | Select-Object -First 1)
    runner_image = $env:ImageVersion
    msvc_version = (Get-Item -LiteralPath (Get-Command cl.exe).Source).VersionInfo.FileVersion
}
$info | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $outputRoot 'build-info.json') -Encoding utf8NoBOM
"source_commit=$sourceCommit" | Out-File -LiteralPath $env:GITHUB_OUTPUT -Append -Encoding utf8

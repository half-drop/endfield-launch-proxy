[CmdletBinding()]
param(
    [string]$RepositoryRoot = '',
    [string]$SourceDirectory = 'src',
    [string]$ResourceFile = 'app.rc',
    [string]$OutputDirectory = 'dist'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) { $RepositoryRoot = Split-Path -Parent $PSScriptRoot }
$repository = [IO.Path]::GetFullPath($RepositoryRoot)
$source = [IO.Path]::GetFullPath((Join-Path $repository $SourceDirectory))
$output = [IO.Path]::GetFullPath((Join-Path $repository $OutputDirectory))
$build = Join-Path $repository 'build'
foreach ($required in @('EndfieldLaunchProxy.cpp', 'proxy.manifest', $ResourceFile)) {
    if (-not (Test-Path -LiteralPath (Join-Path $source $required) -PathType Leaf)) {
        throw "Missing source file: $required"
    }
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer / vswhere.exe was not found.' }
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $installation) { throw 'MSVC x64 build tools were not found.' }
$vcvars = Join-Path ($installation | Select-Object -First 1) 'VC\Auxiliary\Build\vcvars64.bat'
$environment = & $env:ComSpec /d /c "call `"$vcvars`" >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'The Visual Studio x64 environment could not be initialized.' }
foreach ($line in $environment) {
    if ($line -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
    }
}

New-Item -ItemType Directory -Path $build, $output -Force | Out-Null
$object = Join-Path $build 'EndfieldLaunchProxy.obj'
$resource = Join-Path $build 'EndfieldLaunchProxy.res'
$intermediate = Join-Path $build 'EndfieldLaunchProxy.exe'
$manifest = Join-Path $source 'proxy.manifest'

Push-Location $source
try {
    & rc.exe /nologo /c 65001 "/fo$resource" $ResourceFile
    if ($LASTEXITCODE -ne 0) { throw "Resource compilation failed: $LASTEXITCODE" }
    $arguments = @(
        '/nologo', '/EHsc', '/std:c++17', '/MT', '/utf-8', '/O2', '/W4',
        '/DUNICODE', '/D_UNICODE', "/Fo$object", "/Fe$intermediate",
        'EndfieldLaunchProxy.cpp', $resource, '/link', '/SUBSYSTEM:WINDOWS',
        '/MANIFEST:EMBED', "/MANIFESTINPUT:$manifest",
        'user32.lib', 'advapi32.lib', 'shell32.lib', 'ole32.lib',
        'wintrust.lib', 'crypt32.lib'
    )
    & cl.exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "C++ build failed: $LASTEXITCODE" }
}
finally {
    Pop-Location
}

$name = '明日方舟：终末地.exe'
$artifact = Join-Path $output $name
Copy-Item -LiteralPath $intermediate -Destination $artifact -Force
$hash = (Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText((Join-Path $output 'SHA256SUMS.txt'), "$hash  $name`n", [Text.UTF8Encoding]::new($false))
Write-Host "Built: $artifact"
Write-Host "SHA256: $hash"

[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidatePattern('^v\d+\.\d+\.\d+$')][string]$Version,
    [string]$RepositoryRoot = ''
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) { $RepositoryRoot = Split-Path -Parent $PSScriptRoot }
$repository = [IO.Path]::GetFullPath($RepositoryRoot)
$dist = Join-Path $repository 'dist'
$exeName = '明日方舟：终末地.exe'
$exe = Join-Path $dist $exeName
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { throw 'Build the executable before packaging.' }
$fileVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo($exe).FileVersion
if ($fileVersion -ne ($Version.Substring(1) + '.0')) { throw 'Tag version and executable version do not match.' }
$stage = Join-Path $repository 'build/package'
New-Item -ItemType Directory -Force -Path $stage | Out-Null
$packFiles = @(
    @{ Source = $exe; Name = $exeName },
    @{ Source = (Join-Path $repository 'docs/使用说明与验收.txt'); Name = '使用说明与验收.txt' },
    @{ Source = (Join-Path $repository 'LICENSE'); Name = 'LICENSE.txt' },
    @{ Source = (Join-Path $repository 'THIRD_PARTY_NOTICES.md'); Name = 'THIRD_PARTY_NOTICES.md' }
)
$zipInputs = foreach ($file in $packFiles) {
    $destination = Join-Path $stage $file.Name
    Copy-Item -LiteralPath $file.Source -Destination $destination -Force
    $destination
}
$windowsName = "EndfieldLaunchProxy-$Version-windows-x64.zip"
Compress-Archive -LiteralPath $zipInputs -DestinationPath (Join-Path $dist $windowsName) -CompressionLevel Optimal -Force
Push-Location $repository
try {
    $commit = (& git rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Unable to resolve the source commit.' }
    $sourceName = "EndfieldLaunchProxy-$Version-source.zip"
    & git archive --format=zip "--prefix=endfield-launch-proxy-$Version/" "--output=$(Join-Path $dist $sourceName)" HEAD
    if ($LASTEXITCODE -ne 0) { throw 'Source archive creation failed.' }
}
finally { Pop-Location }
$info = [ordered]@{
    version = $Version
    commit = $commit
    platform = 'windows-x64'
    executable = $exeName
    runtime = 'native Win32, MSVC static CRT (/MT), Windows system DLLs'
    configuration = 'Release GUI, asInvoker'
    iconSha256 = (Get-FileHash -LiteralPath (Join-Path $repository 'assets/endfield.ico') -Algorithm SHA256).Hash.ToLowerInvariant()
    executableSha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
    builtAtUtc = [DateTime]::UtcNow.ToString('o')
    actionRun = $env:GITHUB_RUN_ID
    repository = $env:GITHUB_REPOSITORY
}
[IO.File]::WriteAllText((Join-Path $dist 'build-info.json'), ($info | ConvertTo-Json -Depth 3), [Text.UTF8Encoding]::new($false))
$releaseFiles = @($exeName, $windowsName, $sourceName, 'build-info.json')
$sums = foreach ($name in $releaseFiles) {
    $hash = (Get-FileHash -LiteralPath (Join-Path $dist $name) -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $name"
}
[IO.File]::WriteAllText((Join-Path $dist 'SHA256SUMS.txt'), (($sums -join "`n") + "`n"), [Text.UTF8Encoding]::new($false))
Write-Host "Packaged $Version at commit $commit"

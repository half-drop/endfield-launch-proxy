[CmdletBinding()]
param([string]$RepositoryRoot = '')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) { $RepositoryRoot = Split-Path -Parent $PSScriptRoot }
$repository = [IO.Path]::GetFullPath($RepositoryRoot)
$exe = Join-Path $repository 'dist/明日方舟：终末地.exe'
$bytes = [IO.File]::ReadAllBytes($exe)
if ($bytes.Length -lt 1024 -or [BitConverter]::ToUInt16($bytes, 0) -ne 0x5a4d) { throw 'Not a valid PE executable.' }
$pe = [BitConverter]::ToInt32($bytes, 0x3c)
if ($pe -lt 0 -or $pe + 96 -gt $bytes.Length) { throw 'Invalid PE offset.' }
if ([BitConverter]::ToUInt32($bytes, $pe) -ne 0x00004550) { throw 'Missing PE header.' }
if ([BitConverter]::ToUInt16($bytes, $pe + 4) -ne 0x8664) { throw 'Expected an x64 executable.' }
if ([BitConverter]::ToUInt16($bytes, $pe + 24) -ne 0x20b) { throw 'Expected PE32+ format.' }
if ([BitConverter]::ToUInt16($bytes, $pe + 24 + 68) -ne 2) { throw 'Expected Windows GUI subsystem.' }

$optionalHeader = $pe + 24
$optionalSize = [BitConverter]::ToUInt16($bytes, $pe + 20)
$sectionCount = [BitConverter]::ToUInt16($bytes, $pe + 6)
$sectionStart = $optionalHeader + $optionalSize
if ($optionalSize -lt 240 -or $sectionStart + 40 * $sectionCount -gt $bytes.Length) { throw 'Invalid PE section headers.' }
$headerSize = [BitConverter]::ToUInt32($bytes, $optionalHeader + 60)
function Convert-RvaToFileOffset([uint32]$Rva) {
    if ($Rva -lt $headerSize -and $Rva -lt $bytes.Length) { return [int]$Rva }
    for ($sectionIndex = 0; $sectionIndex -lt $sectionCount; $sectionIndex++) {
        $section = $sectionStart + 40 * $sectionIndex
        $virtualSize = [BitConverter]::ToUInt32($bytes, $section + 8)
        $virtualAddress = [BitConverter]::ToUInt32($bytes, $section + 12)
        $rawSize = [BitConverter]::ToUInt32($bytes, $section + 16)
        $rawOffset = [BitConverter]::ToUInt32($bytes, $section + 20)
        $span = [Math]::Max([long]$virtualSize, [long]$rawSize)
        if ($Rva -ge $virtualAddress -and [long]$Rva -lt [long]$virtualAddress + $span) {
            $delta = [long]$Rva - $virtualAddress
            $offset = [long]$rawOffset + $delta
            if ($delta -ge $rawSize -or $offset -ge $bytes.Length) { throw 'PE RVA points outside file-backed section data.' }
            return [int]$offset
        }
    }
    throw "Unmapped PE RVA: $Rva"
}
$importRva = [BitConverter]::ToUInt32($bytes, $optionalHeader + 112 + 8)
$importSize = [BitConverter]::ToUInt32($bytes, $optionalHeader + 112 + 12)
if ($importRva -eq 0 -or $importSize -lt 20) { throw 'Expected an import directory.' }
$imports = [Collections.Generic.List[string]]::new()
$importTerminator = $false
for ($descriptorIndex = 0; $descriptorIndex -lt [Math]::Floor($importSize / 20); $descriptorIndex++) {
    $descriptor = Convert-RvaToFileOffset ([uint32]($importRva + 20 * $descriptorIndex))
    if ($descriptor + 20 -gt $bytes.Length) { throw 'Truncated import descriptor.' }
    $fields = @(0, 4, 8, 12, 16 | ForEach-Object { [BitConverter]::ToUInt32($bytes, $descriptor + $_) })
    if (@($fields | Where-Object { $_ -ne 0 }).Count -eq 0) { $importTerminator = $true; break }
    if ($fields[3] -eq 0) { throw 'Import descriptor has no DLL name.' }
    $nameOffset = Convert-RvaToFileOffset $fields[3]
    $end = $nameOffset
    while ($end -lt $bytes.Length -and $bytes[$end] -ne 0 -and $end - $nameOffset -lt 260) { $end++ }
    if ($end -ge $bytes.Length -or $bytes[$end] -ne 0) { throw 'Invalid imported DLL name.' }
    $dll = [Text.Encoding]::ASCII.GetString($bytes, $nameOffset, $end - $nameOffset)
    if ($dll -match '^(MSVCP|MSVCR|VCRUNTIME|CONCRT|VCOMP|UCRTBASE|API-MS-WIN-CRT-)') { throw "Unexpected dynamic C/C++ runtime dependency: $dll" }
    $imports.Add($dll)
}
if (-not $importTerminator -or $imports.Count -eq 0) { throw 'Invalid or empty PE import table.' }
# This project does not use /DELAYLOAD. Refuse a new delay-import table so it
# cannot introduce a runtime DLL dependency that the normal import scan misses.
if ([BitConverter]::ToUInt32($bytes, $optionalHeader + 112 + 13 * 8) -ne 0) { throw 'Unexpected delay-import table; review its dependencies before release.' }

$version = [Diagnostics.FileVersionInfo]::GetVersionInfo($exe)
if ($version.FileMajorPart -ne 1 -or $version.FileMinorPart -ne 0 -or $version.FileBuildPart -ne 2 -or $version.FilePrivatePart -ne 0) { throw 'Expected fixed file version 1.0.2.0.' }
if ($version.ProductMajorPart -ne 1 -or $version.ProductMinorPart -ne 0 -or $version.ProductBuildPart -ne 2 -or $version.ProductPrivatePart -ne 0) { throw 'Expected fixed product version 1.0.2.0.' }
if ($version.FileVersion -cne '1.0.2.0' -or $version.ProductVersion -cne '1.0.2.0') { throw 'Expected version strings 1.0.2.0.' }
if ($version.OriginalFilename -cne '明日方舟：终末地.exe') { throw 'OriginalFilename does not match the published filename.' }

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class ReleaseResourceCheck {
    [DllImport("kernel32.dll", EntryPoint="LoadLibraryExW", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern IntPtr LoadLibraryEx(string path, IntPtr file, uint flags);
    [DllImport("kernel32.dll", EntryPoint="FindResourceW", SetLastError=true)]
    public static extern IntPtr FindResource(IntPtr module, IntPtr name, IntPtr type);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern uint SizeofResource(IntPtr module, IntPtr resource);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern IntPtr LoadResource(IntPtr module, IntPtr resource);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern IntPtr LockResource(IntPtr resource);
    [DllImport("kernel32.dll")]
    public static extern bool FreeLibrary(IntPtr module);
}
'@
function Read-EmbeddedResource([IntPtr]$Module, [int]$Name, [int]$Type) {
    $resourceInfo = [ReleaseResourceCheck]::FindResource($Module, [IntPtr]$Name, [IntPtr]$Type)
    if ($resourceInfo -eq [IntPtr]::Zero) { throw "Missing resource type=$Type id=$Name." }
    $length = [ReleaseResourceCheck]::SizeofResource($Module, $resourceInfo)
    $loaded = [ReleaseResourceCheck]::LoadResource($Module, $resourceInfo)
    $pointer = [ReleaseResourceCheck]::LockResource($loaded)
    if ($length -eq 0 -or $pointer -eq [IntPtr]::Zero) { throw "Cannot read resource type=$Type id=$Name." }
    $data = [byte[]]::new($length)
    [Runtime.InteropServices.Marshal]::Copy($pointer, $data, 0, $data.Length)
    return ,$data
}
$module = [ReleaseResourceCheck]::LoadLibraryEx($exe, [IntPtr]::Zero, 2)
if ($module -eq [IntPtr]::Zero) { throw 'Executable resources could not be loaded.' }
try {
    $group = Read-EmbeddedResource $module 101 14
    $ico = [IO.File]::ReadAllBytes((Join-Path $repository 'assets/endfield.ico'))
    if ($ico.Length -lt 6 -or $group.Length -lt 6) { throw 'Truncated icon directory.' }
    if ([BitConverter]::ToUInt16($ico, 0) -ne 0 -or [BitConverter]::ToUInt16($ico, 2) -ne 1 -or [BitConverter]::ToUInt16($group, 0) -ne 0 -or [BitConverter]::ToUInt16($group, 2) -ne 1) { throw 'Expected ICO / group-icon directory headers.' }
    $frameCount = [BitConverter]::ToUInt16($ico, 4)
    if ($frameCount -eq 0 -or [BitConverter]::ToUInt16($group, 4) -ne $frameCount) { throw 'Embedded icon frame count differs from assets/endfield.ico.' }
    if ($group.Length -ne 6 + 14 * $frameCount -or $ico.Length -lt 6 + 16 * $frameCount) { throw 'Invalid icon directory length.' }
    for ($frame = 0; $frame -lt $frameCount; $frame++) {
        $icoEntry = 6 + 16 * $frame
        $groupEntry = 6 + 14 * $frame
        for ($field = 0; $field -lt 12; $field++) {
            if ($ico[$icoEntry + $field] -ne $group[$groupEntry + $field]) { throw "Icon frame $frame metadata differs from assets/endfield.ico." }
        }
        $frameSize = [BitConverter]::ToUInt32($ico, $icoEntry + 8)
        $frameOffset = [BitConverter]::ToUInt32($ico, $icoEntry + 12)
        if ($frameSize -eq 0 -or [long]$frameOffset + $frameSize -gt $ico.Length) { throw "Invalid ICO frame $frame bounds." }
        $frameId = [BitConverter]::ToUInt16($group, $groupEntry + 12)
        $embeddedFrame = Read-EmbeddedResource $module $frameId 3
        if ($embeddedFrame.Length -ne $frameSize) { throw "Embedded icon frame $frame length differs." }
        $expectedFrame = [Convert]::ToBase64String($ico, $frameOffset, $frameSize)
        if ([Convert]::ToBase64String($embeddedFrame) -cne $expectedFrame) { throw "Embedded icon frame $frame bytes differ from assets/endfield.ico." }
    }

    $manifestBytes = Read-EmbeddedResource $module 1 24
    $manifestStream = [IO.MemoryStream]::new($manifestBytes, $false)
    $settings = [Xml.XmlReaderSettings]::new()
    $settings.DtdProcessing = [Xml.DtdProcessing]::Prohibit
    $settings.XmlResolver = $null
    $reader = [Xml.XmlReader]::Create($manifestStream, $settings)
    try {
        $manifestXml = [Xml.XmlDocument]::new()
        $manifestXml.XmlResolver = $null
        $manifestXml.Load($reader)
        $levels = $manifestXml.SelectNodes("//*[local-name()='requestedExecutionLevel']")
        if ($levels.Count -ne 1 -or $levels[0].GetAttribute('level') -cne 'asInvoker' -or $levels[0].GetAttribute('uiAccess') -cne 'false') { throw 'Expected an asInvoker manifest with uiAccess=false.' }
    }
    finally {
        $reader.Dispose()
        $manifestStream.Dispose()
    }
}
finally { [ReleaseResourceCheck]::FreeLibrary($module) | Out-Null }

$safeGame = Join-Path $repository 'build/this-game-does-not-exist/Endfield.exe'
if (Test-Path -LiteralPath $safeGame) { throw 'The safe failure-path target unexpectedly exists.' }
$log = Join-Path $repository 'build/ci-failure-check.log'
$arguments = '"--game=' + $safeGame + '" "--log=' + $log + '"'
$process = Start-Process -FilePath $exe -ArgumentList $arguments -PassThru -WindowStyle Hidden
try {
    if (-not $process.WaitForExit(15000)) {
        $process.Kill()
        throw 'Safe failure-path check did not finish within 15 seconds.'
    }
    if ($process.ExitCode -ne 12) { throw "Expected game-file-missing exit 12; received $($process.ExitCode)." }
}
finally { $process.Dispose() }
$logText = Get-Content -LiteralPath $log -Raw
if ($logText -notmatch 'reason=game_file_missing') { throw 'The expected failure reason was not recorded.' }
if ($logText -match 'official_parent_created_suspended|game_created') { throw 'Safe failure check unexpectedly created a parent or game.' }
$expected = ((Get-Content -LiteralPath (Join-Path $repository 'dist/SHA256SUMS.txt') -Raw) -split '\s+', 2)[0]
$actual = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actual -cne $expected) { throw 'SHA256SUMS.txt does not match the executable.' }
Write-Host "Verified x64 GUI format; imports: $($imports -join ', ')"
Write-Host "Verified all $frameCount official icon frames, version 1.0.2.0, exact OriginalFilename, asInvoker manifest, safe missing-file exit, and SHA-256."

#requires -Version 7.4
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$request = [Console]::In.ReadLine() | ConvertFrom-Json -AsHashtable -ErrorAction Stop
$root = Split-Path $PSScriptRoot
if ($request.schema -ne 1 -or $request.command -ne 'asan' -or $request.projectRoot -ne $root -or
    $request.dryRun -isnot [bool] -or $request.parameters -isnot [hashtable] -or $request.parameters.Count) {
    throw 'Expected a schema 1 asan request with no parameters.'
}
. (Join-Path $root '.dd/core.ps1')
foreach ($module in Get-DDRuntimeModules) { . (Join-Path $root ".dd/$module") }
$platform = Get-DDPlatform
$preset = if ($IsWindows) { 'windows-asan' } else { 'linux-asan' }
$results = @()
if (-not $request.dryRun) {
    $manifest = Read-DDManifest $root -ForBuild
    Assert-DDDependencies $root
    $manifest.build[$platform].asan = $preset
    $results += Invoke-DDBuild $root $manifest asan @{}
    $results += Invoke-DDTests $root $manifest asan @{ label = '^cfindata$' } @()
}
@{ schema = 1; data = @{ preset = $preset; results = $results }; files = @("build/$platform/asan") } |
    ConvertTo-Json -Depth 15 -Compress | Write-Output

#requires -Version 7.4
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$root = Split-Path $PSScriptRoot
$pwsh = Join-Path $PSHOME $(if ($IsWindows) { 'pwsh.exe' } else { 'pwsh' })
function Invoke-Driver([string[]]$Arguments, [int]$ExpectedExit = 0) {
    $output = & $pwsh -NoProfile -File (Join-Path $root 'dd.ps1') @Arguments --project $root --non-interactive --json
    $code = $LASTEXITCODE
    $result = $output | ConvertFrom-Json -AsHashtable -ErrorAction Stop
    if ($code -ne $ExpectedExit -or $result.exitCode -ne $code -or $result.ok -ne ($code -eq 0)) {
        throw "Unexpected dd status: $output"
    }
    return $result
}
$pin = Get-Content (Join-Path $root 'docs/dd-upstream.json') -Raw | ConvertFrom-Json -AsHashtable
if ($pin.version -ne '0.2.2' -or $pin.revision -ne '114890d721907dee64651371947f49dd33dad27c' -or
    $pin.repository -ne 'https://github.com/ZacWalk/dd') { throw 'Unexpected upstream release' }
$paths = @('dd.ps1') + @(Get-ChildItem (Join-Path $root '.dd') -File -Recurse -Force |
    ForEach-Object { [IO.Path]::GetRelativePath($root, $_.FullName).Replace('\', '/') } |
    Where-Object { -not $_.StartsWith('.dd/state/') })
if (Compare-Object ($paths | Sort-Object) ($pin.files.Keys | Sort-Object)) { throw 'Incomplete runtime inventory' }
foreach ($path in $paths) {
    if ((Get-FileHash (Join-Path $root $path) -Algorithm SHA256).Hash -cne $pin.files[$path]) {
        throw "Vendored runtime modified: $path"
    }
}
$attributes = Get-Content (Join-Path $root '.gitattributes') -Raw
if ($attributes -notmatch '/dd\.ps1 -text' -or $attributes -notmatch '/\.dd/\*\* -text') {
    throw 'Missing byte-stable checkout rules'
}
if ($attributes -notmatch '/tests/fixtures/\*\* -text') {
    throw 'Fixture payload bytes must survive Git line-ending conversion'
}
$manifest = Import-PowerShellDataFile (Join-Path $root 'dd.psd1')
$presets = Get-Content (Join-Path $root 'CMakePresets.json') -Raw | ConvertFrom-Json -AsHashtable
if ($manifest.project.type -ne 'library' -or $manifest.project.'default-target' -ne 'cfindata-cli' -or
    $manifest.dependencies.owner -ne 'application') { throw 'Unexpected library/CLI ownership contract' }
foreach ($platform in @('x64-windows', 'x64-linux')) {
    foreach ($config in @('debug', 'release')) {
        $name = $manifest.build[$platform][$config]
        $configure = @($presets.configurePresets | Where-Object name -eq $name)
        if ($configure.Count -ne 1 -or $configure[0].binaryDir -ne ('${sourceDir}/build/' + $platform + '/' + $config)) {
            throw "Unexpected $platform $config configure path"
        }
        foreach ($phase in @('buildPresets', 'testPresets')) {
            $preset = @($presets[$phase] | Where-Object name -eq $name)
            if ($preset.Count -ne 1 -or $preset[0].configurePreset -ne $name) { throw "Incorrect $phase reference" }
        }
    }
}
$targets = (Invoke-Driver @('targets')).data.targets
$library = @($targets | Where-Object id -eq 'cfindata')
$cli = @($targets | Where-Object id -eq 'cfindata-cli')
if ($targets.Count -ne 2 -or $library.Count -ne 1 -or $cli.Count -ne 1 -or
    $library[0].kind -ne 'library' -or $library[0].runnable -or -not $cli[0].runnable -or
    $library[0].cmakeTarget -ne 'cfindata' -or $cli[0].cmakeTarget -ne 'cfindata_cli') {
    throw 'Unexpected native targets'
}
foreach ($verb in @('run', 'launch')) {
    $response = Invoke-Driver @($verb, 'cfindata') 2
    if (($response.errors -join ' ') -notmatch 'library.*no executable') { throw 'Library execution not refused' }
}
$plan = (Invoke-Driver @('asan', '--dry-run')).data.result
$expected = if ($IsWindows) { 'windows-asan' } else { 'linux-asan' }
if ($plan.preset -ne $expected -or $plan.results.Count) { throw 'Incorrect sanitizer dry-run' }
$null = Invoke-Driver @('asan', '--unknown', 'true', '--dry-run') 2
$cmake = Get-Content (Join-Path $root 'CMakeLists.txt') -Raw
if ($cmake -notmatch 'LANGUAGES C\)' -or $cmake -match 'platform-h|LANGUAGES CXX|\.cpp') {
    throw 'Library must remain plain C and independent of platform-h'
}
foreach ($legacy in @('research.slnx', 'src/main.cpp', 'src/main.vcxproj', 'src/main.vcxproj.filters',
    'src/main.vcxproj.user', 'src/json_parser.h', 'src/macro_scoring.h', 'src/prompt.h',
    'src/llm_client.h', 'src/model.h', 'src/stock_json.h', 'src/macro_data.h')) {
    if (Test-Path (Join-Path $root $legacy)) { throw "Legacy application file remains: $legacy" }
}
if ((Test-Path (Join-Path $root 'vcpkg.json')) -or
    (Get-Content (Join-Path $root 'CMakePresets.json') -Raw) -match 'toolchainFile|VCPKG_' -or
    $cmake -match 'LibArchive|OpenSSL') {
    throw 'Library presets must not restore application-owned archive or hash dependencies'
}
$null = Invoke-Driver @('build', 'invalid') 2
$workflow = Get-Content (Join-Path $root '.github/workflows/ci.yml') -Raw
foreach ($required in @('  push:', '  pull_request:', '  workflow_dispatch:', 'contents: read',
    'actions/checkout@v5', 'persist-credentials: false', 'windows-latest', 'ubuntu-latest',
    'g++ cmake ninja-build libcurl4-openssl-dev',
    'dd.ps1 dep install --non-interactive', 'dd.ps1 doctor --non-interactive',
    'dd.ps1 test --non-interactive', 'dd.ps1 asan --yes --non-interactive',
    'if: failure()', 'tmp/**/dd-ctest-*.xml', 'build/**/Testing/Temporary/*.log')) {
    if (-not $workflow.Contains($required)) { throw "CI contract missing: $required" }
}
if ($workflow -match 'vcpkg|libarchive-dev|libssl-dev|continue-on-error') {
    throw 'CI must use only the native transport dependency and preserve test failures'
}
$badge = 'https://github.com/ZacWalk/libcfindata/actions/workflows/ci.yml'
if (-not (Get-Content (Join-Path $root 'README.md') -Raw).Contains("($badge/badge.svg)]($badge)")) {
    throw 'README badge must target this repository and workflow'
}
$drivers = @(Get-ChildItem -LiteralPath $root -File -Filter '*.ps1')
if ($drivers.Count -ne 1 -or $drivers[0].Name -ne 'dd.ps1' -or (Test-Path (Join-Path $root 'dd.sh'))) {
    throw 'Only immutable dd may remain as a root driver'
}
foreach ($path in @('dd.ps1', 'tests/dd-adoption.ps1') + @($manifest.commands.Values.script | Sort-Object -Unique)) {
    $tokens = $null; $parseErrors = $null
    $null = [Management.Automation.Language.Parser]::ParseFile((Join-Path $root $path), [ref]$tokens, [ref]$parseErrors)
    if ($parseErrors.Count) { throw "PowerShell parse failed: $path : $parseErrors" }
}
Write-Host 'PASS pinned runtime, C-only library/CLI, presets, sanitizer adapter, CI/badge contract and script parsing'

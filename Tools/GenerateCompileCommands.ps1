<#
.SYNOPSIS
    Generates compile_commands.json at the repository root for clangd.

.DESCRIPTION
    Asks MSBuild to evaluate OblivionReloaded.vcxproj (no build is performed) and converts
    every ClCompile item into a clang-cl command line: the project's defines, include
    directories and forced include, plus the toolset's system include path (MSVC STL,
    Windows SDK, DirectX SDK) as /imsvc so clangd resolves headers exactly like cl.exe.
    The target is forced to 32-bit x86 so MSVC __asm blocks parse.

    Re-run after adding or removing source files or changing compiler settings.

.PARAMETER Configuration
    Project configuration to evaluate (Release or Debug).
#>
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$project = Join-Path $root 'OblivionReloaded\OblivionReloaded.vcxproj'
$output = Join-Path $root 'compile_commands.json'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsPath = & $vswhere -latest -prerelease -products * -requires Microsoft.Component.MSBuild -property installationPath
if (-not $vsPath) { throw 'No Visual Studio installation with MSBuild found.' }
$msbuild = Join-Path $vsPath 'MSBuild\Current\Bin\MSBuild.exe'
$clangCl = Join-Path $vsPath 'VC\Tools\Llvm\x64\bin\clang-cl.exe'

$evaluation = & $msbuild $project /nologo "/p:Configuration=$Configuration" /p:Platform=Win32 "/p:SolutionDir=$root\\" `
    -getProperty:IncludePath -getProperty:VCToolsVersion -getItem:ClCompile
if ($LASTEXITCODE -ne 0) { throw "MSBuild evaluation failed:`n$($evaluation -join "`n")" }
$evaluation = ($evaluation -join "`n") | ConvertFrom-Json

function Split-MSBuildList([string]$list) {
    return @($list -split ';' | Where-Object { $_ -and $_ -notmatch '^%\(' })
}

$toolsVersion = [version]$evaluation.Properties.VCToolsVersion
$msCompatibility = "19.$($toolsVersion.Minor)"

$systemIncludes = Split-MSBuildList $evaluation.Properties.IncludePath |
    Where-Object { Test-Path -LiteralPath $_ } |
    Select-Object -Unique

$languageStandards = @{
    'stdcpp14'     = 'c++14'
    'stdcpp17'     = 'c++17'
    'stdcpp20'     = 'c++20'
    'stdcpp23'     = 'c++23preview'
    'stdcpplatest' = 'c++latest'
}
$runtimeLibraries = @{
    'MultiThreaded'         = '/MT'
    'MultiThreadedDebug'    = '/MTd'
    'MultiThreadedDll'      = '/MD'
    'MultiThreadedDebugDll' = '/MDd'
}

# No error limit: a header opened on its own is skipped inside the forced Framework.h chain,
# and the resulting cascade must not abort the parse before the header's own body.
# The two warnings are constructs cl.exe accepts that clang rejects by default.
$clangCompatibility = @(
    '/clang:-ferror-limit=0',
    '-Wno-enum-enum-conversion',
    '-Wno-address-of-temporary'
)

$entries = foreach ($item in $evaluation.Items.ClCompile) {
    if ($item.ExcludedFromBuild -eq 'true') { continue }

    $arguments = [System.Collections.Generic.List[string]]::new()
    $arguments.AddRange([string[]]@(
        $clangCl, '/c', '/nologo', '/TP', '--target=i686-pc-windows-msvc',
        "-fms-compatibility-version=$msCompatibility"))
    $arguments.AddRange([string[]]$clangCompatibility)

    if ($languageStandards.ContainsKey($item.LanguageStandard)) {
        $arguments.Add("/std:$($languageStandards[$item.LanguageStandard])")
    }
    if ($runtimeLibraries.ContainsKey($item.RuntimeLibrary)) {
        $arguments.Add($runtimeLibraries[$item.RuntimeLibrary])
    }
    if ($item.ExceptionHandling -eq 'Sync') { $arguments.Add('/EHsc') }
    if ($item.TreatWChar_tAsBuiltInType -eq 'true') { $arguments.Add('/Zc:wchar_t') }
    $arguments.Add($(if ($item.ConformanceMode -eq 'true') { '/permissive-' } else { '/permissive' }))

    foreach ($define in Split-MSBuildList $item.PreprocessorDefinitions) { $arguments.Add("/D$define") }
    foreach ($include in Split-MSBuildList $item.AdditionalIncludeDirectories) { $arguments.Add("/I$include") }
    foreach ($forced in Split-MSBuildList $item.ForcedIncludeFiles) { $arguments.Add("/FI$forced") }
    foreach ($include in $systemIncludes) { $arguments.Add("/imsvc$include") }

    $arguments.Add($item.FullPath)

    [ordered]@{
        directory = Split-Path -Parent $project
        file      = $item.FullPath
        arguments = $arguments.ToArray()
    }
}

$json = ConvertTo-Json -InputObject @($entries) -Depth 4
[System.IO.File]::WriteAllText($output, $json, [System.Text.UTF8Encoding]::new($false))
Write-Host "Wrote $(@($entries).Count) entries to $output"

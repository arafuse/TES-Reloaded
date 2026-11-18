$root = Resolve-Path "$PSScriptRoot\..\.."
$vcvars = 'C:\Development\Microsoft\Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat'
$exe = Join-Path $env:TEMP 'GrassTrailMapTest.exe'
$cmd = "`"$vcvars`" >nul && cd /d `"$root`" && cl /nologo /EHsc /O2 /W4 /I TESReloaded\Core Tools\Tests\GrassTrailMapTest.cpp TESReloaded\Core\GrassTrailMap.cpp /Fe:`"$exe`" /Fo:`"$env:TEMP/`" && `"$exe`""
cmd /c $cmd
exit $LASTEXITCODE

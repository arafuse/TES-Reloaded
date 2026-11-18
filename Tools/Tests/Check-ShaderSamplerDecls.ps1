# TextureManager::LoadTexture binds a TESR_ sampler only if its declaration is in the TOP-LEVEL
# shader source (ShaderRecord reads just that file). A TESR_ sampler sampled anywhere in a shader's
# include tree but declared only in an include is silently left unbound. This fails on any such shader.
$root = Resolve-Path "$PSScriptRoot\..\..\OblivionReloaded\Shaders"
$failures = 0

function Get-IncludeTree([string]$File, [string]$TopDir, [hashtable]$Seen) {
	if ($Seen.ContainsKey($File)) { return }
	$Seen[$File] = $true
	foreach ($m in [regex]::Matches((Get-Content -Raw $File), '#include\s+"([^"]+)"')) {
		$rel = $m.Groups[1].Value
		$candidates = @((Join-Path $TopDir $rel), (Join-Path (Split-Path $File) $rel))
		foreach ($c in $candidates) {
			if (Test-Path $c) { Get-IncludeTree (Resolve-Path $c).Path $TopDir $Seen; break }
		}
	}
}

Get-ChildItem $root -Recurse -Include *.pso.hlsl, *.vso.hlsl | ForEach-Object {
	$top = $_.FullName
	$topText = Get-Content -Raw $top
	$seen = @{}
	Get-IncludeTree $top $_.DirectoryName $seen
	$used = @{}
	foreach ($f in $seen.Keys) {
		foreach ($m in [regex]::Matches((Get-Content -Raw $f), '\btex(?:2D|2Dlod|2Dproj|2Dbias|2Dgrad|3D|CUBE)\w*\s*\(\s*(TESR_\w+)')) {
			$used[$m.Groups[1].Value] = $true
		}
	}
	foreach ($name in $used.Keys) {
		if ($topText -notmatch "sampler\w*\s+$name\b") {
			Write-Output "$($top.Substring($root.Path.Length + 1)): $name is sampled but not declared in the top-level file"
			$failures++
		}
	}
}

if ($failures) { Write-Output "ShaderSamplerDecls: $failures failure(s)"; exit 1 }
Write-Output "ShaderSamplerDecls: all shaders declare their TESR_ samplers at top level"

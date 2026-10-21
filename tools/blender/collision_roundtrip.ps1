# Collision round trip: exports each model, runs it through Blender, imports it again and prints the collision result.
# Usage: collision_roundtrip.ps1 -models <model.geo>,... -gameRoot <Tabula Rasa folder> [-blender <blender.exe>]
param([string[]]$models, [string]$gameRoot = $env:RASA_GAMEROOT,
      [string]$blender = "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe")
if (-not $gameRoot) { throw "pass -gameRoot (the Tabula Rasa folder) or set RASA_GAMEROOT" }
Set-Location (Resolve-Path "$PSScriptRoot\..\..")
$S = Join-Path $env:TEMP "rasaedit_gltf_test"; New-Item -ItemType Directory -Force "$S\gltf" | Out-Null
$B = $blender
$root = $gameRoot
$env:RASA_NOENTITIES = "1"; $env:RASA_NOTERRAIN = "1"; $env:RASA_GLTFPREVIEW = ""; $env:RASA_GLTFIMPORT = ""
foreach ($m in $models) {
    $stem = [IO.Path]::GetFileNameWithoutExtension($m)
    $own = "$S\gltf\col_$stem"; $re = "$S\gltf\colre_$stem"
    if (Test-Path $own) { Remove-Item -Recurse -Force $own }
    if (Test-Path $re) { Remove-Item -Recurse -Force $re }
    $env:RASA_GLTFEXPORT = $m; $env:RASA_GLTFOUT = "$own\$stem.gltf"
    & .\out\build\release\viewport_test.exe $root "$root\data\game.zip" adv_bootcamp NUL 16 16 0 0 0 1 0 1 hide 2>&1 | Select-String "gltf export"
    $env:RASA_GLTFEXPORT = ""
    & $B --background --factory-startup --python "$PSScriptRoot\blender_check.py" -- "$own\$stem.gltf" "$S\gltf\col_$stem.png" "$re\$stem.gltf" 2>&1 | Select-String "RESULT collection|Traceback"
    foreach ($src in @("$own\$stem.gltf", "$re\$stem.gltf")) {
        "=== $src"
        $env:RASA_GLTFIMPORT = $src
        & .\out\build\release\viewport_test.exe $root "$root\data\game.zip" adv_bootcamp NUL 16 16 0 0 0 1 0 1 hide 2>&1 | Select-String "\.geo \(|collision|import warning"
    }
    $env:RASA_GLTFIMPORT = ""
}

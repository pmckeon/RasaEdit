param([string[]]$models)
Set-Location "D:\Projects\Tabula Rasa\RasaEdit"
$S = Join-Path $env:TEMP "rasaedit_gltf_test"; New-Item -ItemType Directory -Force "$S\gltf" | Out-Null
$B = "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe"
$root = "E:\Games\Tabula Rasa 1.16.5.0"
$env:RASA_NOENTITIES = "1"; $env:RASA_NOTERRAIN = "1"; $env:RASA_GLTFPREVIEW = ""; $env:RASA_GLTFIMPORT = ""
foreach ($m in $models) {
    $stem = [IO.Path]::GetFileNameWithoutExtension($m)
    $own = "$S\gltf\col_$stem"; $re = "$S\gltf\colre_$stem"
    if (Test-Path $own) { Remove-Item -Recurse -Force $own }
    if (Test-Path $re) { Remove-Item -Recurse -Force $re }
    $env:RASA_GLTFEXPORT = $m; $env:RASA_GLTFOUT = "$own\$stem.gltf"
    & .\out\build\release\viewport_test.exe $root "$root\data\game.zip" adv_bootcamp NUL 16 16 0 0 0 1 0 1 hide 2>&1 | Select-String "gltf export"
    $env:RASA_GLTFEXPORT = ""
    & $B --background --factory-startup --python "D:\Projects\Tabula Rasa\RasaEdit\tools\blender\blender_check.py" -- "$own\$stem.gltf" "$S\gltf\col_$stem.png" "$re\$stem.gltf" 2>&1 | Select-String "RESULT collection|Traceback"
    foreach ($src in @("$own\$stem.gltf", "$re\$stem.gltf")) {
        "=== $src"
        $env:RASA_GLTFIMPORT = $src
        & .\out\build\release\viewport_test.exe $root "$root\data\game.zip" adv_bootcamp NUL 16 16 0 0 0 1 0 1 hide 2>&1 | Select-String "\.geo \(|collision|import warning"
    }
    $env:RASA_GLTFIMPORT = ""
}

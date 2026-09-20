# Phase 4 diagnostic: structural validation of exported .gfmodel animation data
# across a random sample, mirroring Phase 3 PHASE3_FINDINGS §8's format check.
# Not part of the shipped pipeline -- run ad hoc, see PHASE4_FINDINGS for the
# recorded results.
param(
    [string]$OutDir = "G:\GIT\GF NIF Exporter\out",
    [int]$SampleSize = 500
)

$models = Get-ChildItem -Path $OutDir -Filter *.gfmodel -Recurse
Write-Output "Total .gfmodel files: $($models.Count)"

$rand = New-Object System.Random(42)
$sample = $models | Sort-Object { $rand.Next() } | Select-Object -First $SampleSize

$issues = 0
$clipsChecked = 0
$tracksChecked = 0
$nonMonotonicTimes = 0
$badQuatNorm = 0
$maxQuatNormErr = 0.0
$filesWithAnimation = 0

foreach ($m in $sample) {
    $json = Get-Content $m.FullName -Raw | ConvertFrom-Json
    $binPath = Join-Path $m.DirectoryName ($json.binary)
    if (-not (Test-Path $binPath)) { $issues++; Write-Output "MISSING BIN: $($m.FullName)"; continue }
    $binLen = (Get-Item $binPath).Length
    if ($binLen -ne $json.binaryByteLength) { $issues++; Write-Output "BIN LEN MISMATCH: $($m.FullName)" }

    if ($json.animations.Count -eq 0) { continue }
    $filesWithAnimation++
    $bytes = [System.IO.File]::ReadAllBytes($binPath)

    $boneCount = if ($json.skeletons.Count -gt 0) { $json.skeletons[0].boneCount } else { 0 }

    foreach ($clip in $json.animations) {
        $clipsChecked++
        foreach ($t in $clip.tracks) {
            $tracksChecked++
            if ($t.boneIndex -lt -1 -or ($boneCount -gt 0 -and $t.boneIndex -ge $boneCount)) {
                $issues++; Write-Output "BONE INDEX OOB: $($m.Name) $($t.boneName) idx=$($t.boneIndex) boneCount=$boneCount"
            }
            foreach ($chName in @('translation','rotation','scale')) {
                $ch = $t.$chName
                $itemSize = $ch.values.itemSize
                if ($ch.count -ne $ch.times.count -or $ch.count -ne $ch.values.count) {
                    $issues++; Write-Output "COUNT MISMATCH: $($m.Name) $chName"
                }
                $timesEnd = $ch.times.byteOffset + $ch.count * 4
                $valuesEnd = $ch.values.byteOffset + $ch.count * $itemSize * 4
                if ($timesEnd -gt $binLen -or $valuesEnd -gt $binLen) {
                    $issues++; Write-Output "OOB: $($m.Name) $chName"
                    continue
                }
                if ($ch.count -gt 1) {
                    $prevTime = [System.BitConverter]::ToSingle($bytes, $ch.times.byteOffset)
                    for ($i = 1; $i -lt $ch.count; $i++) {
                        $tm = [System.BitConverter]::ToSingle($bytes, $ch.times.byteOffset + $i * 4)
                        if ($tm -lt $prevTime) { $nonMonotonicTimes++ }
                        $prevTime = $tm
                    }
                }
                if ($chName -eq 'rotation' -and $ch.count -gt 0) {
                    for ($i = 0; $i -lt $ch.count; $i++) {
                        $off = $ch.values.byteOffset + $i * 16
                        $x = [System.BitConverter]::ToSingle($bytes, $off)
                        $y = [System.BitConverter]::ToSingle($bytes, $off + 4)
                        $z = [System.BitConverter]::ToSingle($bytes, $off + 8)
                        $w = [System.BitConverter]::ToSingle($bytes, $off + 12)
                        $norm = [Math]::Sqrt($x*$x + $y*$y + $z*$z + $w*$w)
                        $err = [Math]::Abs($norm - 1.0)
                        if ($err -gt $maxQuatNormErr) { $maxQuatNormErr = $err }
                        if ($err -gt 0.01) {
                            $badQuatNorm++
                            if ($badQuatNorm -le 10) {
                                Write-Output "BAD QUAT: $($m.Name) clip=$($clip.name) origin=$($clip.originFile) resampled=$($clip.wasResampledFromBSpline) bone=$($t.boneName) key=$i norm=$norm err=$err"
                            }
                        }
                    }
                }
            }
        }
    }
}

Write-Output ""
Write-Output "Sampled: $($sample.Count) files, $filesWithAnimation with animation"
Write-Output "Clips checked: $clipsChecked, tracks checked: $tracksChecked"
Write-Output "Structural issues: $issues"
Write-Output "Non-monotonic key times: $nonMonotonicTimes"
Write-Output "Quaternion norm errors > 0.01: $badQuatNorm (max err $maxQuatNormErr)"

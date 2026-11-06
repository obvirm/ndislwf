param(
    [string]$Vmrun,
    [string]$Vmx,
    [string]$GuestUser,
    [string]$GuestPass,
    [string]$ReleaseDir = "E:\project\ndislwf\src\ndislwf\x64\Release",
    [string]$GuestBin = "C:\ndislwf\bin",
    [string]$WrapperReinstall = "C:\ndislwg\h_rdrv.cmd",
    [string]$PullDir = "E:\project\ndislwf\tests\guest",
    [switch]$Bind,
    [switch]$SkipReinstall
)

foreach ($pair in @(
        @('Vmrun', 'NL_VMRUN'),
        @('Vmx', 'NL_VMX'),
        @('GuestUser', 'NL_GUEST_USER'),
        @('GuestPass', 'NL_GUEST_PASS'))) {
    if (-not (Get-Variable $pair[0]).Value) { Set-Variable $pair[0] ([Environment]::GetEnvironmentVariable($pair[1])) }
}
if (-not $Vmrun -or -not $Vmx -or -not $GuestUser -or -not $GuestPass) {
    throw "butuh param -Vmrun/-Vmx/-GuestUser/-GuestPass atau env NL_VMRUN/NL_VMX/NL_GUEST_USER/NL_GUEST_PASS"
}

$ErrorActionPreference = "Stop"

function Copy-And-Wait {
    param([string]$HostFile, [string]$GuestFile, [int]$Sec = 3)

    & $Vmrun -T ws -gu $GuestUser -gp $GuestPass copyFileFromHostToGuest `
        $Vmx $HostFile $GuestFile | Out-Null
    Start-Sleep -Seconds $Sec
    $out = & $Vmrun -T ws -gu $GuestUser -gp $GuestPass fileExistsInGuest $Vmx $GuestFile
    # vmrun fileExistsInGuest = "The file exists." | involucrate errors
    if (-not ($out -join " " -match "file exists")) { throw "copy gagal: $GuestFile tidak ada" }
}

function Pull-File {
    param([string]$GuestFile, [string]$HostFile, [int]$Sec = 10)

    & $Vmrun -T ws -gu $GuestUser -gp $GuestPass copyFileFromGuestToHost `
        $Vmx $GuestFile $HostFile | Out-Null
    Start-Sleep -Seconds $Sec
}

$files = @("NDISLWF.sys", "ndislwf.inf")
foreach ($f in $files) {
    if (-not (Test-Path (Join-Path $ReleaseDir $f))) {
        throw "berkas build hilang: $ReleaseDir\$f"
    }
}

# cat berada di sub-direktori yang sama bernama project
$CatPath = Join-Path $ReleaseDir "ndislwf\ndislwf.cat"
if (-not (Test-Path $CatPath)) { throw "berkas build hilang: $CatPath" }

Write-Output "[$(Get-Date -Format HH:mm:ss)] $ReleaseDir -> $GuestBin"

$sha = [System.Security.Cryptography.SHA256]::Create()
foreach ($f in ($files | ForEach-Object { Join-Path $ReleaseDir $_ }) + @($CatPath)) {
    $hb = $sha.ComputeHash([System.IO.File]::ReadAllBytes($f))
    $hash = ($hb | ForEach-Object { $_.ToString("x2") }) -join ""
    Write-Output ("  local {0,-12} sha256 {1}..." -f (Split-Path $f -Leaf), $hash.Substring(0, 16))
}

if ($SkipReinstall) {
    Write-Output "reinstall dilewati (-SkipReinstall)"
    return
}

Copy-And-Wait (Join-Path $ReleaseDir "NDISLWF.sys") "$GuestBin\NDISLWF.sys" 4
Copy-And-Wait (Join-Path $ReleaseDir "ndislwf.inf") "$GuestBin\ndislwf.inf" 3
Copy-And-Wait $CatPath "$GuestBin\ndislwf.cat" 3

Write-Output "[$(Get-Date -Format HH:mm:ss)] reinstall via $WrapperReinstall"
& $Vmrun -T ws -gu $GuestUser -gp $GuestPass runProgramInGuest $Vmx $WrapperReinstall | Out-Null
Start-Sleep -Seconds 20

Pull-File "C:\ndislwf\h_reinstall_out.txt" (Join-Path $PullDir "h_reinstall_out.txt")

$outFile = Join-Path $PullDir "h_reinstall_out.txt"
$finalLine = Select-String -Path $outFile -Pattern "hash DRIVERS sys final:\s*(.*)" | Select-Object -Last 1
if (-not $finalLine) { throw "tidak menemukan baris 'hash DRIVERS sys final' di $outFile" }

$guestHash = ($finalLine.Matches[0].Groups[1].Value -replace "\s", "") -replace "0x", ""
$hostHash = (Get-FileHash (Join-Path $ReleaseDir "NDISLWF.sys") -Algorithm SHA256).Hash.ToLower()
Write-Output "host sys hash : $hostHash"
Write-Output "guest sys hash: $guestHash"
if ($guestHash -eq $hostHash) { Write-Output "SAMA: driver guest = build host" }
else { throw "BEDA: driver guest BUKAN build host (redeploy gagal)" }

if ($Bind) {
    Write-Output "[$(Get-Date -Format HH:mm:ss)] bind ulang (h_nlbind.cmd)"
    & $Vmrun -T ws -gu $GuestUser -gp $GuestPass runProgramInGuest $Vmx "C:\ndislwg\h_nlbind.cmd" | Out-Null
    Start-Sleep -Seconds 25
    Pull-File "C:\ndislwf\nlbind.txt" (Join-Path $PullDir "nlbind.txt")
    Write-Output "  tarik: $PullDir\nlbind.txt"
}

Write-Output "OK $((Get-Date).ToString('HH:mm:ss'))"

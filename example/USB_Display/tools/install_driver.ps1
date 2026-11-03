<#
    Install / re-sign the SiFli USB Display (IddCx) driver.

    Run in an elevated PowerShell:
        powershell -ExecutionPolicy Bypass -File tools\install_driver.ps1

    What it does:
      1. locate Inf2Cat.exe / signtool.exe from the newest Windows Kits
      2. regenerate xfz1986_usb_graphic.cat  (Inf2Cat)
      3. make sure a self-signed code-signing cert exists in LocalMachine\My
         and is trusted (Root + TrustedPublisher)
      4. sign the catalog with signtool
      5. pnputil /add-driver ... /install

    Notes:
      - Test signing must be ON (bcdedit /set testsigning on) + reboot,
        otherwise the kernel/UMDF refuses the self-signed catalog.
      - The INF matches USB\VID_303A&PID_2986&MI_00 only.
        Interface 1 (touch) is handled by the in-box HID driver, no INF needed.
#>
param(
    [string]$DriverDir = (Join-Path (Split-Path -Parent $PSScriptRoot) "driver"),
    [string]$CertSubject = "CN=SiFli USB Display Test Cert",
    [string]$InfName = "xfz1986_usb_graphic.inf"
)

$ErrorActionPreference = "Stop"

function Assert-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $p = New-Object Security.Principal.WindowsPrincipal($id)
    if (-not $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "Please run this script as Administrator."
    }
}

function Find-KitTool([string]$name) {
    $root = "C:\Program Files (x86)\Windows Kits\10\bin"
    if (-not (Test-Path $root)) { throw "Windows Kits not found: $root" }
    $cand = Get-ChildItem $root -Recurse -Filter $name -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match "\\x86\\$name$" } |
            Sort-Object FullName -Descending
    if (-not $cand) { throw "$name not found under $root" }
    return $cand[0].FullName
}

Assert-Admin

if (-not (Test-Path $DriverDir)) { throw "Driver dir not found: $DriverDir" }
$inf = Join-Path $DriverDir $InfName
if (-not (Test-Path $inf)) { throw "INF not found: $inf" }

Write-Host "== driver dir ==" -ForegroundColor Cyan
Get-ChildItem $DriverDir | Format-Table Name, Length, LastWriteTime -AutoSize

# ---- test signing state (informational) ----
$ts = (bcdedit /enum "{current}" | Select-String "testsigning")
Write-Host "== boot option ==" -ForegroundColor Cyan
if ($ts) { Write-Host $ts.ToString() } else { Write-Host "testsigning not set -> run: bcdedit /set testsigning on  (then reboot)" }

# ---- 1. Inf2Cat ----
$inf2cat = Find-KitTool "Inf2Cat.exe"
Write-Host "== Inf2Cat ==" -ForegroundColor Cyan
& $inf2cat /driver:"$DriverDir" /os:10_X64 /verbose
if ($LASTEXITCODE -ne 0) { throw "Inf2Cat failed ($LASTEXITCODE)" }

# ---- 2. certificate ----
Write-Host "== certificate ==" -ForegroundColor Cyan
$cert = Get-ChildItem Cert:\LocalMachine\My | Where-Object { $_.Subject -eq $CertSubject } | Select-Object -First 1
if (-not $cert) {
    Write-Host "creating self-signed cert: $CertSubject"
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $CertSubject `
        -CertStoreLocation Cert:\LocalMachine\My `
        -KeyUsage DigitalSignature -KeyLength 2048 -HashAlgorithm SHA256 `
        -NotAfter (Get-Date).AddYears(10)
}
Write-Host ("thumbprint: " + $cert.Thumbprint)

foreach ($store in @("Root", "TrustedPublisher")) {
    $path = "Cert:\LocalMachine\$store"
    $found = Get-ChildItem $path | Where-Object { $_.Thumbprint -eq $cert.Thumbprint }
    if (-not $found) {
        $tmp = Join-Path $env:TEMP ("sifli_" + $store + ".cer")
        Export-Certificate -Cert $cert -FilePath $tmp -Force | Out-Null
        Import-Certificate -FilePath $tmp -CertStoreLocation $path | Out-Null
        Remove-Item $tmp -Force
        Write-Host "imported into $store"
    } else {
        Write-Host "already in $store"
    }
}

# ---- 3. sign cat ----
$cat = Join-Path $DriverDir ([System.IO.Path]::GetFileNameWithoutExtension($InfName) + ".cat")
if (-not (Test-Path $cat)) { throw "catalog not generated: $cat" }
$signtool = Find-KitTool "signtool.exe"
Write-Host "== signtool ==" -ForegroundColor Cyan
& $signtool sign /sm /s My /n "SiFli USB Display Test Cert" /fd SHA256 /a "$cat"
if ($LASTEXITCODE -ne 0) { throw "signtool failed ($LASTEXITCODE)" }

# ---- 4. install ----
Write-Host "== pnputil ==" -ForegroundColor Cyan
pnputil /add-driver "$inf" /install
if ($LASTEXITCODE -ne 0) { Write-Host "pnputil returned $LASTEXITCODE (a non-zero code is common when the device is not plugged in)" -ForegroundColor Yellow }

Write-Host ""
Write-Host "Done. If the screen still shows the old driver:" -ForegroundColor Green
Write-Host "  1) plug the board out / in"
Write-Host "  2) or: pnputil /enum-drivers  -> find the old oemNN.inf -> pnputil /delete-driver oemNN.inf /uninstall"
Write-Host "  3) verify with: python tools\diag_usb_display.py"

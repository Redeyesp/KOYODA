$ErrorActionPreference = "Stop"
Write-Host "Cleaning any old full-screen charge source files (safe if already absent)..."
1..6 | ForEach-Object {
    $p = "main\koyoda_charge_$_.c"
    if (Test-Path $p) { Remove-Item -Force $p }
}
Write-Host "Done."

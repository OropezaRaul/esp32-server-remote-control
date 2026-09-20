# Helper: inicializa el entorno ESP-IDF v5.3 y ejecuta idf.py con los argumentos dados.
# Uso: cd tools/windows; .\idf.ps1 build
#      cd tools/windows; .\idf.ps1 -p COM13 flash monitor
#      cd tools/windows; .\idf.ps1 menuconfig
#
# Si existe sdkconfig.credentials, sus lineas no-comentadas se anexan al
# sdkconfig (gitignored) despues de `reconfigure`, para que los secretos
# (WiFi pass, Tailscale key, bot token, shutdown secret) tengan efecto.

$ErrorActionPreference = 'Stop'

# Cambiar al root del proyecto (dos niveles arriba de tools/windows/)
$projectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $projectRoot

if (-not (Test-Path -LiteralPath "C:\esp\esp-idf\export.bat")) {
    Write-Host "[ERROR] ESP-IDF no encontrado en C:\esp\esp-idf. Instalar primero." -ForegroundColor Red
    exit 1
}

# Capturar variables de entorno del export.bat y aplicarlas a este proceso
$envDump = cmd.exe /c "call `"C:\esp\esp-idf\export.bat`" >NUL && set" 2>&1
foreach ($line in $envDump) {
    if ($line -match '^([^=]+)=(.*)$') {
        $name = $matches[1]
        $value = $matches[2]
        if ($name -in @('IDF_PATH','IDF_PYTHON_ENV_PATH','IDF_TOOLS_PATH') -or $name.StartsWith('IDF_')) {
            [System.Environment]::SetEnvironmentVariable($name, $value, 'Process')
        }
    }
}

$pathLine = ($envDump | Select-String -Pattern '^Path=' | Select-Object -First 1)
if ($pathLine) {
    $newPath = $pathLine -replace '^Path=',''
    [System.Environment]::SetEnvironmentVariable('Path', $newPath, 'Process')
    $env:Path = $newPath
}

if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    Write-Host "[ERROR] idf.py no esta en PATH tras sourcing." -ForegroundColor Red
    exit 1
}

# Si la primera accion es reconfigure/set-target/merge-config y tenemos
# sdkconfig.credentials, anexar las lineas no-comentadas al sdkconfig antes
# de invocar idf.py. Asi los secretos se cargan sin commitearlos.
$needsMerge = ($args.Count -gt 0) -and ($args[0] -in @('reconfigure','set-target','menuconfig') -or $args -contains 'reconfigure')
if (Test-Path -LiteralPath "sdkconfig.credentials") {
    Write-Host "[merge] aplicando sdkconfig.credentials -> sdkconfig"
    $creds = Get-Content "sdkconfig.credentials" | Where-Object {
        $_ -notmatch '^\s*#' -and $_.Trim() -ne ''
    }
    Add-Content -Path "sdkconfig" -Value "`n# === from sdkconfig.credentials ==="
    Add-Content -Path "sdkconfig" -Value $creds
}

&idf.py @args
exit $LASTEXITCODE
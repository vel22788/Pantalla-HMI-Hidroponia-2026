param([string]$Salida = (Join-Path $PSScriptRoot 'salida'))
$ErrorActionPreference = 'Stop'
# Solo compila. No abre puertos, no carga placas y no borra calibraciones.
$hmiRaiz = $PSScriptRoot
$hmiSalida = [IO.Path]::GetFullPath($Salida)
New-Item -ItemType Directory -Path $hmiSalida -Force | Out-Null
$hmiFqbn = 'esp32:esp32:esp32s3:FlashMode=qio,FlashSize=4M,PartitionScheme=huge_app,PSRAM=opi,CPUFreq=240,UploadSpeed=115200'
& arduino-cli compile --fqbn $hmiFqbn --libraries (Join-Path $hmiRaiz 'librerias') --build-path (Join-Path $hmiSalida 'build') --output-dir (Join-Path $hmiSalida 'binarios') (Join-Path $hmiRaiz 'FirmwareHMI')
if ($LASTEXITCODE -ne 0) { throw 'Fallo la compilacion de la pantalla.' }

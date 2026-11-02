# Pantalla HMI de la plataforma hidropónica — 2026 — Ángel Velásquez

Universidad del Valle de Guatemala · Diseño e Innovación en Ingeniería.

Interfaz para **Elecrow DIS08070H V3.0 de 7 pulgadas**, con ESP32-S3,
resolución **800 × 480** y táctil GT911. Presenta las variables del sistema,
controles manuales, gráficas, programación de eventos, historial y reloj.
Se comunica con el controlador central mediante el protocolo ASCII V2.

## Estructura

```text
FirmwareHMI/
  FirmwareHMI.ino          Panel RGB, táctil, LVGL y transporte UART
  touch.h                 Adaptador del táctil GT911
  build_opt.h             Opciones de fuentes y widgets para la compilación
  integrar_exportacion.py Integración de futuras exportaciones SquareLine
  src/
    app/                  Cliente UART, datos, controles y configuración
    formulario/           Editor y modelo de eventos
    reloj/                Indicador y editor de fecha/hora del central
    ui/                   Pantallas, callbacks, imágenes y recursos exportados
librerias/                Dependencias y configuración de LVGL
proyecto_squareline/      Proyecto editable de las cinco pantallas
Compilar.ps1              Compilación con rutas relativas
```

Abrir **`FirmwareHMI/FirmwareHMI.ino`** para trabajar con el firmware.
El proyecto editable se abre con **`proyecto_squareline/BaseHidroponica.spj`**.
La aplicación, el formulario y el reloj programados en C/C++ se conservan en
sus carpetas para mantenerlos independientes de una nueva exportación gráfica.

## Dependencias y configuración

- Arduino IDE o Arduino CLI.
- Plataforma ESP32 de Espressif **3.3.10**.
- Placa **ESP32S3 Dev Module**, flash **4 MB**, modo **QIO**, partición
  **Huge APP**, **PSRAM OPI**, CPU **240 MHz**, carga **115200 baudios**.
- Bibliotecas incluidas: **LVGL 9.5.0**, **LovyanGFX 1.2.21**,
  **PCA9557 1.0.0** y **TAMC_GT911 1.0.2**.
- SquareLine Studio **1.6.2** para el diseño editable.

Conservar `librerias/lv_conf.h` y `FirmwareHMI/build_opt.h`. Este último contiene
opciones de compilación para Montserrat 16/30 y Dropdown; el núcleo ESP32 las
incorpora al compilar. Las imágenes y fuentes convertidas a C forman parte de
la interfaz y deben permanecer junto al sketch.

## Compilación

### PowerShell y Arduino CLI

Con Arduino CLI instalado, ejecutar desde la raíz del repositorio:

```powershell
arduino-cli core install esp32:esp32@3.3.10 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
./Compilar.ps1
```

El primer comando instala la plataforma. El script compila usando las
bibliotecas incluidas y guarda los archivos generados en `salida/`.
Puede elegirse otra ubicación con `./Compilar.ps1 -Salida RUTA`.

### Arduino IDE

Copiar las cuatro carpetas de `librerias/` y `lv_conf.h` a `libraries` del
sketchbook. Abrir el `.ino`, seleccionar la placa y los ajustes anteriores,
y compilar. Mantener la estructura interna de `FirmwareHMI/`.

## Cargar el firmware

Comprobar el puerto USB de la pantalla y cerrar el monitor o puente que lo
ocupe. Desde Arduino IDE utilizar Subir con la configuración indicada.
Con Arduino CLI, después de compilar:

```text
arduino-cli board list
arduino-cli upload --fqbn esp32:esp32:esp32s3:FlashMode=qio,FlashSize=4M,PartitionScheme=huge_app,PSRAM=opi,CPUFreq=240,UploadSpeed=115200 --port COM_PUERTO_REAL --input-dir ./salida/binarios ./FirmwareHMI
```

Sustituir `COM_PUERTO_REAL` por el puerto correspondiente.

## Comunicación y controles

UART0 se dedica al protocolo a **115200 baudios**: HMI **TX43/RX44** a
central **RX41/TX42**, con GND común. El transporte por puente USB de PC se
gestiona con su herramienta separada.

Los controles solicitan órdenes al central. La interfaz actualiza su estado
mediante respuestas y consultas; una aceptación de orden es distinta de su
finalización. Cada modificación pendiente se sigue por ID y las solicitudes
sin confirmación se reconcilian consultando su resultado.

Los datos conservan calidad y antigüedad por campo. El pH con calidad
PROVISIONAL se presenta con `~`; el reloj utiliza la fecha y hora del RTC
central. Para editarlo, tocar la hora en Principal o engranaje → Fecha y hora.
Guardar exige MANUAL y procesos detenidos; la edición de campos es un borrador
hasta recibir la confirmación del central.

## Ajustar el cliente

Editar `FirmwareHMI/src/app/hmi_config.h` para baudios, pines, intervalos de
consulta, tiempos de espera y capacidades de las vistas. Los tiempos de riego
y movimiento se configuran en el [firmware del central](https://github.com/vel22788/ESP32-Central-Hidroponia-2026).

Los comentarios del código describen el flujo de mensajes, estados, unidades,
callbacks, editor de eventos y manejo del reloj.

## Exportar desde SquareLine

Exportar la interfaz a una carpeta aparte e integrar los archivos:

```text
python FirmwareHMI/integrar_exportacion.py RUTA_DE_LA_EXPORTACION
```

El integrador conecta los callbacks y adapta los iconos necesarios, conservando
`src/app`, `src/formulario` y `src/reloj`. Revisar los cambios y recompilar.
El proyecto editable contiene Principal, Modo Manual, Variables, Programación
e Historial, junto con sus recursos gráficos.

## Licencias

Conservar la licencia de la fuente en
[`FirmwareHMI/src/formulario/OFL-Montserrat.txt`](FirmwareHMI/src/formulario/OFL-Montserrat.txt)
y los avisos de las bibliotecas:

- [LVGL](librerias/lvgl/LICENCE.txt).
- [LovyanGFX](librerias/LovyanGFX/license.txt).
- [TAMC GT911](librerias/TAMC_GT911/LICENSE).
- PCA9557 conserva sus avisos en los archivos distribuidos por el proveedor.

La carpeta `librerias/lvgl/src/misc/cache/` contiene la implementación del gestor
de caché de LVGL. `ja.map` de LovyanGFX es un mapa de caracteres de fuente.
Ambos forman parte de las dependencias.

## Revisión de entrega

Etiqueta: `entrega-2026-11-02-hmi-documentada`.
El código completo también corresponde a las carpetas de entrega en OneDrive,
además del enlace a este repositorio.

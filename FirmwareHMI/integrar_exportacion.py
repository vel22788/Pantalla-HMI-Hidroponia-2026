"""Actualiza únicamente src/ui; conserva aplicación, formulario y originales."""
from pathlib import Path
import argparse
import hashlib
import json
import re
from datetime import datetime

BASE = Path(__file__).resolve().parent
UI = (BASE / "src/ui").resolve()
CUERPOS = {
    "sincronizarUmbralDesdeSlider": """    (void)e;
    int32_t valor = lv_slider_get_value(ui_sldUmbralHumedad);
    if(lv_spinbox_get_value(ui_spnUmbralHumedad) != valor)
        lv_spinbox_set_value(ui_spnUmbralHumedad, valor);""",
    "sincronizarUmbralDesdeSpinbox": """    (void)e;
    int32_t valor = lv_spinbox_get_value(ui_spnUmbralHumedad);
    if(lv_slider_get_value(ui_sldUmbralHumedad) != valor)
        lv_slider_set_value(ui_sldUmbralHumedad, valor, LV_ANIM_OFF);""",
    "openAddEventPopup": "    hmi_app_abrir_evento(e);",
    "aplicarUmbralHumedad": "    hmi_app_aplicar_umbral(e);",
    "cargarHistorial": "    hmi_app_cargar_historial(e);",
    "actualizarHistorial": "    hmi_app_actualizar_historial(e);",
}


def conectar_eventos(texto):
    for nombre, cuerpo in CUERPOS.items():
        patron = re.compile(r"void\s+" + nombre + r"\s*\(lv_event_t\s*\*\s*e\)\s*\{")
        inicio = patron.search(texto)
        if not inicio:
            raise ValueError(f"Falta la función exportada {nombre}")
        # Los callbacks generados son cuerpos C sencillos; contar llaves permite
        # volver a aplicar la integración a un cuerpo previamente integrado.
        fin, profundidad = inicio.end(), 1
        while fin < len(texto) and profundidad:
            profundidad += (texto[fin] == "{") - (texto[fin] == "}")
            fin += 1
        if profundidad:
            raise ValueError(f"Cuerpo incompleto: {nombre}")
        texto = texto[:inicio.end()] + "\n" + cuerpo + "\n}" + texto[fin:]
    if '#include "../app/hmi_app.h"' not in texto:
        texto = texto.replace('#include "ui.h"', '#include "ui.h"\n#include "../app/hmi_app.h"', 1)
    return texto


def convertir_icono(texto):
    """I8 BGRA -> RGB565 + plano A8, conservando geometría y transparencia.

    El dibujador local no transforma estos dos I8 al escalarlos. La conversión
    se limita a la copia C de trabajo; no modifica el bitmap del usuario.
    """
    if "LV_COLOR_FORMAT_I8" not in texto:
        return texto
    ancho = int(re.search(r"\.header\.w\s*=\s*(\d+)", texto).group(1))
    alto = int(re.search(r"\.header\.h\s*=\s*(\d+)", texto).group(1))
    mapa = re.search(r"(_map\[\]\s*=\s*\{)(.*?)(\};)", texto, re.S)
    if not mapa:
        raise ValueError("No se encontró mapa I8")
    datos = bytes(int(h, 16) for h in re.findall(r"0x([0-9a-fA-F]{2})", mapa.group(2)))
    if len(datos) != 1024 + ancho * alto:
        raise ValueError("Mapa I8 con dimensiones inesperadas")
    rgb, alfa = bytearray(), bytearray()
    for indice in datos[1024:]:
        b, g, r, a = datos[indice * 4:indice * 4 + 4]
        color = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        rgb.extend((color & 255, color >> 8))
        alfa.append(a)
    plano = rgb + alfa
    lineas = ["  " + ", ".join(f"0x{v:02x}" for v in plano[i:i+24]) + ","
              for i in range(0, len(plano), 24)]
    texto = texto[:mapa.start(2)] + "\n" + "\n".join(lineas) + "\n" + texto[mapa.end(2):]
    texto = texto.replace("LV_COLOR_FORMAT_I8", "LV_COLOR_FORMAT_RGB565A8")
    texto = re.sub(r"\.data_size\s*=\s*\d+", f".data_size = {len(plano)}", texto)
    texto = texto.replace(".header.magic =", f".header.stride = {ancho * 2},\n  .header.magic =")
    return texto


def integrar(origen):
    origen = origen.resolve(strict=True)
    # Resolver y verificar antes de escribir o quitar cualquier archivo.
    if UI != (BASE / "src/ui").resolve() or not UI.is_relative_to(BASE):
        raise ValueError("Destino fuera del firmware")
    if origen == UI or origen.is_relative_to(UI):
        raise ValueError("La exportación debe estar fuera del destino")
    archivos = {p.name: p for p in origen.iterdir() if p.is_file() and
                (p.name.startswith("ui") and p.suffix in (".c", ".h") or
                 p.name in ("CMakeLists.txt", "filelist.txt"))}
    necesarios = ("ui.c", "ui.h", "ui_events.c", "ui_VariablesPantalla.c",
                  "ui_PrincipalPantalla.c", "ui_ModoManualPantalla.c",
                  "ui_ProgramacionPantalla.c", "ui_HistorialPantalla.c")
    if not all(nombre in archivos for nombre in necesarios):
        raise ValueError("Exportación incompleta o estructura distinta de V2")
    # Validar todo antes de tocar la copia; no confiar en posición de callbacks.
    eventos = conectar_eventos(archivos["ui_events.c"].read_text(encoding="utf-8"))
    convertidos = {nombre: convertir_icono(archivos[nombre].read_text(encoding="utf-8"))
                   for nombre in ("ui_img_ph_png.c", "ui_img_variables_png.c") if nombre in archivos}
    manifiesto = {nombre: hashlib.sha256(p.read_bytes()).hexdigest()
                  for nombre, p in sorted(archivos.items())}
    UI.mkdir(parents=True, exist_ok=True)
    cambios = 0
    for nombre, p in archivos.items():
        destino = UI / nombre
        if destino.resolve().parent != UI:
            raise ValueError("Destino no válido")
        if nombre == "ui_events.c":
            contenido = eventos.replace("\n", "\r\n").encode("utf-8")
        elif nombre in convertidos:
            contenido = convertidos[nombre].replace("\n", "\r\n").encode("utf-8")
        else:
            contenido = p.read_bytes()
        if not destino.exists() or destino.read_bytes() != contenido:
            destino.write_bytes(contenido)
            cambios += 1
    for viejo in UI.iterdir():
        if viejo.is_file() and viejo.name not in archivos and viejo.resolve().parent == UI:
            if viejo.name.startswith("ui") and viejo.suffix in (".c", ".h"):
                viejo.unlink()
    (BASE / "exportacion_integrada.json").write_text(json.dumps(
        {"origen": str(origen), "ultima_modificacion_origen": datetime.fromtimestamp(
            max(p.stat().st_mtime for p in archivos.values())).isoformat(timespec="seconds"),
         "sha256_originales": manifiesto}, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"Integrados {len(archivos)} archivos en {UI}; {cambios} cambios. Originales conservados.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("origen", type=Path, help="Carpeta externa exportada por SquareLine")
    integrar(parser.parse_args().origen)

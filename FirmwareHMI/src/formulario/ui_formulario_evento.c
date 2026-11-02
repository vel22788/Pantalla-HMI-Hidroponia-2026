/**
 * @file ui_formulario_evento.c
 * Editor de eventos construido por codigo: hora, metodo, destino, duracion,
 * dias y habilitacion. Tiene un borrador separado de la agenda confirmada.
 * Guardar valida, bloquea controles y espera respuesta con token e ID central.
 * Un timeout puede dejar una escritura incierta: cerrar la vista no la cancela
 * ni vuelve a enviarla. No se maneja riego ni se persiste agenda en la HMI.
 * La fuente ui_fuente_es_18 aporta caracteres espanoles; conservar su licencia.
 */
/* Formulario independiente de SquareLine, para LVGL 9.5 y 800 x 480.
 * No controla actuadores ni inventa una agenda local.
 */
#include "ui_formulario_evento.h"
#include <stdio.h>
#include <string.h>

#if LVGL_VERSION_MAJOR != 9
#error "Este formulario requiere LVGL 9."
#endif
#if !LV_USE_DROPDOWN || !LV_USE_SPINBOX || !LV_USE_SWITCH || !LV_USE_BUTTON || !LV_USE_LABEL
#error "Habilitar Dropdown, Spinbox, Switch, Button y Label en lv_conf.h."
#endif
#if !LV_FONT_MONTSERRAT_18 || !LV_FONT_MONTSERRAT_24
#error "Habilitar Montserrat 18 y 24 en lv_conf.h."
#endif

#define COLOR_VERDE 0x269E3F
#define COLOR_AZUL 0x2196F3
#define COLOR_TEXTO 0x263238
#define COLOR_GRIS 0x63717A
#define COLOR_ERROR 0xB3261E

LV_FONT_DECLARE(ui_fuente_es_18);

/**
 * Estado de un unico formulario: widgets, ID de edicion, token pendiente y
 * borrador. No es la base persistente de eventos del equipo.
 */
typedef struct {
    lv_obj_t * fondo;
    lv_obj_t * titulo;
    lv_obj_t * controles[20];
    unsigned cantidad_controles;
    lv_obj_t * hora;
    lv_obj_t * minuto;
    lv_obj_t * metodo;
    lv_obj_t * destino;
    lv_obj_t * duracion;
    lv_obj_t * unidad;
    lv_obj_t * habilitado;
    lv_obj_t * texto_habilitado;
    lv_obj_t * dias[7];
    lv_obj_t * estado;
    lv_obj_t * cancelar;
    lv_obj_t * guardar;
    bool cerrando;
    bool incierta;
    uint32_t editar_id;
    uint32_t solicitud;
    hmi_evento_t borrador;
} formulario_t;

static formulario_t formulario;
static ui_evento_enviar_cb_t enviar_cb;
static ui_evento_guardado_cb_t guardado_cb;
static void * contexto_cb;
static uint32_t secuencia_solicitud;
static void campo_cambiado(lv_event_t * e);

static void posicion(lv_obj_t * obj, int x, int y, int ancho, int alto)
{
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, ancho, alto);
}

static lv_obj_t * texto(lv_obj_t * parent, const char * contenido,
                        int x, int y, int ancho)
{
    lv_obj_t * obj = lv_label_create(parent);
    lv_label_set_text(obj, contenido);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_width(obj, ancho);
    lv_obj_set_style_text_font(obj, &ui_fuente_es_18, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(COLOR_TEXTO), 0);
    return obj;
}

static void registrar(lv_obj_t * obj)
{
    if(formulario.cantidad_controles < sizeof(formulario.controles) / sizeof(formulario.controles[0]))
        formulario.controles[formulario.cantidad_controles++] = obj;
    lv_obj_add_event_cb(obj, campo_cambiado, LV_EVENT_VALUE_CHANGED, NULL);
}

static lv_obj_t * boton(lv_obj_t * parent, const char * contenido,
                        int x, int y, int ancho, int alto, uint32_t color)
{
    lv_obj_t * obj = lv_button_create(parent);
    posicion(obj, x, y, ancho, alto);
    lv_obj_set_style_radius(obj, 10, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_text_color(obj, lv_color_white(), 0);
    lv_obj_set_style_text_font(obj, &ui_fuente_es_18, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t * label = lv_label_create(obj);
    lv_label_set_text(label, contenido);
    lv_obj_center(label);
    registrar(obj);
    return obj;
}

static lv_obj_t * selector(lv_obj_t * parent, const char * opciones,
                           int x, int y, int ancho)
{
    lv_obj_t * obj = lv_dropdown_create(parent);
    lv_dropdown_set_options(obj, opciones);
    posicion(obj, x, y, ancho, 44);
    lv_obj_set_style_text_font(obj, &ui_fuente_es_18, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(COLOR_TEXTO), 0);
    lv_obj_set_style_radius(obj, 9, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(0xD6DDE1), 0);
    lv_obj_set_style_bg_color(obj, lv_color_white(), 0);
    lv_obj_t * lista = lv_dropdown_get_list(obj);
    lv_obj_set_style_text_font(lista, &ui_fuente_es_18, 0);
    lv_obj_set_style_max_height(lista, 220, 0);
    lv_dropdown_set_dir(obj, LV_DIR_BOTTOM);
    registrar(obj);
    return obj;
}

static void estado(const char * mensaje, bool error)
{
    lv_label_set_text(formulario.estado, mensaje);
    lv_obj_set_style_text_color(formulario.estado,
                               lv_color_hex(error ? COLOR_ERROR : COLOR_GRIS), 0);
}

static void campo_cambiado(lv_event_t * e)
{
    (void)e;
    if(formulario.estado == NULL || formulario.solicitud || formulario.cerrando) return;
    estado("Los eventos se ejecutan\nsolo en modo automático.", false);
}

static void actualizar_destino(void)
{
    bool lluvia = lv_dropdown_get_selected(formulario.metodo) == HMI_METODO_LLUVIA;
    if(lluvia) lv_obj_add_state(formulario.destino, LV_STATE_DISABLED);
    else lv_obj_remove_state(formulario.destino, LV_STATE_DISABLED);
}

static void controles_bloquear(bool bloquear)
{
    for(unsigned i = 0; i < formulario.cantidad_controles; i++) {
        if(bloquear) lv_obj_add_state(formulario.controles[i], LV_STATE_DISABLED);
        else lv_obj_remove_state(formulario.controles[i], LV_STATE_DISABLED);
    }
    if(!bloquear) actualizar_destino();
}

/**
 * Solicita borrado asincrono para no destruir widgets dentro de su callback.
 * La notificacion LV_EVENT_DELETE limpia despues los punteros de la vista.
 */
static void cerrar(void)
{
    if(formulario.fondo == NULL || formulario.cerrando) return;
    formulario.cerrando = true;
    controles_bloquear(true);
    lv_obj_delete_async(formulario.fondo);
}

static void eliminado(lv_event_t * e)
{
    (void)e;
    memset(&formulario, 0, sizeof(formulario));
}

static void metodo_cambiado(lv_event_t * e)
{
    (void)e;
    if(formulario.solicitud || formulario.cerrando) return;
    if(lv_dropdown_get_selected(formulario.metodo) == HMI_METODO_LLUVIA)
        lv_dropdown_set_options(formulario.destino, "Vertical");
    else
        lv_dropdown_set_options(formulario.destino, "Elegir...\nMódulo 1\nMódulo 2\nAmbos");
    lv_dropdown_set_selected(formulario.destino, 0);
    actualizar_destino();
}

static void habilitado_cambiado(lv_event_t * e)
{
    (void)e;
    lv_label_set_text(formulario.texto_habilitado,
                     lv_obj_has_state(formulario.habilitado, LV_STATE_CHECKED)
                     ? "Habilitado" : "Deshabilitado");
}

static void duracion_paso(lv_event_t * e)
{
    if(formulario.solicitud || formulario.cerrando) return;
    bool aumentar = (uintptr_t)lv_event_get_user_data(e) != 0;
    lv_spinbox_set_step(formulario.duracion, 1);
    if(aumentar) lv_spinbox_increment(formulario.duracion);
    else lv_spinbox_decrement(formulario.duracion);
    lv_spinbox_set_cursor_pos(formulario.duracion, 0);
}

static void todos_clicked(lv_event_t * e)
{
    (void)e;
    if(formulario.solicitud || formulario.cerrando) return;
    bool todos = true;
    for(unsigned i = 0; i < 7; i++) todos &= lv_obj_has_state(formulario.dias[i], LV_STATE_CHECKED);
    for(unsigned i = 0; i < 7; i++) {
        if(todos) lv_obj_remove_state(formulario.dias[i], LV_STATE_CHECKED);
        else lv_obj_add_state(formulario.dias[i], LV_STATE_CHECKED);
    }
}

static void cancelar_clicked(lv_event_t * e)
{
    (void)e;
    if(!formulario.solicitud || formulario.incierta) cerrar();
}

/**
 * Construye evento RAM y convierte minutos a segundos una sola vez.
 * La primera opcion Elegir no es un valor real de hora/metodo/unidad.
 * Devuelve NULL si valida o mensaje que se presenta sin transmitir.
 */
static const char * leer_borrador(hmi_evento_t * evento)
{
    memset(evento, 0, sizeof(*evento));
    evento->id = formulario.editar_id;
    unsigned hora = lv_dropdown_get_selected(formulario.hora);
    unsigned minuto = lv_dropdown_get_selected(formulario.minuto);
    evento->hora = hora ? (uint8_t)(hora - 1) : UINT8_MAX;
    evento->minuto = minuto ? (uint8_t)(minuto - 1) : UINT8_MAX;
    evento->metodo = (hmi_metodo_t)lv_dropdown_get_selected(formulario.metodo);
    evento->destino = evento->metodo == HMI_METODO_LLUVIA
                      ? HMI_DESTINO_VERTICAL
                      : (hmi_destino_t)lv_dropdown_get_selected(formulario.destino);
    unsigned unidad = lv_dropdown_get_selected(formulario.unidad);
    if(unidad == 0) return "Elegí la unidad de duración.";
    evento->duracion_s = (uint32_t)lv_spinbox_get_value(formulario.duracion) * (unidad == 2 ? 60U : 1U);
    for(unsigned i = 0; i < 7; i++) {
        if(lv_obj_has_state(formulario.dias[i], LV_STATE_CHECKED)) evento->dias |= (uint8_t)(1U << i);
    }
    evento->habilitado = lv_obj_has_state(formulario.habilitado, LV_STATE_CHECKED);
    return hmi_evento_validar(evento);
}

/**
 * Bloquea controles, asigna token local y entrega borrador al callback.
 * Solo la confirmacion correspondiente puede dar por guardado. El callback
 * podria confirmar de inmediato; no sobrescribir ese estado al retornar.
 */
static void guardar_clicked(lv_event_t * e)
{
    (void)e;
    if(formulario.solicitud || formulario.cerrando) return;
    const char * error = leer_borrador(&formulario.borrador);
    if(error != NULL) { estado(error, true); return; }
    if(enviar_cb == NULL) {
        estado("El envío al central todavía no está conectado.", true);
        return;
    }
    secuencia_solicitud++;
    if(secuencia_solicitud == 0) secuencia_solicitud++;
    formulario.solicitud = secuencia_solicitud;
    uint32_t solicitud = formulario.solicitud;
    controles_bloquear(true);
    estado("Esperando confirmación del central...", false);
    bool enviada = enviar_cb(&formulario.borrador, solicitud, contexto_cb);
    /* El callback puede confirmar inmediatamente; no sobrescribir ese estado. */
    if(!enviada && formulario.solicitud == solicitud)
        ui_evento_confirmar(solicitud, false, 0, "No se pudo enviar. Revisá la conexión.");
}

void ui_evento_configurar(ui_evento_enviar_cb_t enviar,
                         ui_evento_guardado_cb_t guardado, void * contexto)
{
    /* No cambiar el destino de callbacks de una operacion en curso. */
    if(formulario.solicitud) return;
    enviar_cb = enviar;
    guardado_cb = guardado;
    contexto_cb = contexto;
}

/**
 * Crea el editor 800x480 sobre la pantalla recibida. Rechaza duplicados
 * y mantiene el borrador separado de la agenda hasta confirmar Guardar.
 */
void ui_evento_abrir(lv_obj_t * parent)
{
    if(parent == NULL || formulario.fondo != NULL) return;
    memset(&formulario, 0, sizeof(formulario));
    formulario.fondo = lv_obj_create(parent);
    posicion(formulario.fondo, 0, 0, 800, 480);
    lv_obj_remove_flag(formulario.fondo, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(formulario.fondo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(formulario.fondo, 0, 0);
    lv_obj_set_style_border_width(formulario.fondo, 0, 0);
    lv_obj_set_style_pad_all(formulario.fondo, 0, 0);
    lv_obj_set_style_bg_color(formulario.fondo, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(formulario.fondo, LV_OPA_60, 0);
    lv_obj_add_event_cb(formulario.fondo, eliminado, LV_EVENT_DELETE, NULL);

    lv_obj_t * panel = lv_obj_create(formulario.fondo);
    posicion(panel, 20, 16, 760, 448);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(panel, 16, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0xF4F7F9), 0);

    lv_obj_t * cabecera = lv_obj_create(panel);
    posicion(cabecera, 0, 0, 760, 50);
    lv_obj_remove_flag(cabecera, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(cabecera, 0, 0);
    lv_obj_set_style_border_width(cabecera, 0, 0);
    lv_obj_set_style_pad_all(cabecera, 0, 0);
    lv_obj_set_style_bg_color(cabecera, lv_color_hex(COLOR_VERDE), 0);
    lv_obj_t * titulo = texto(cabecera, "Agregar evento de riego", 24, 11, 700);
    formulario.titulo = titulo;
    lv_obj_set_style_text_font(titulo, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(titulo, lv_color_white(), 0);
    lv_obj_t * subtitulo = texto(panel, "Elegí cuándo y dónde se ejecutará el riego.", 24, 62, 710);
    lv_obj_set_style_text_color(subtitulo, lv_color_hex(COLOR_GRIS), 0);

    texto(panel, "Hora de inicio", 24, 94, 200);
    char opciones_hora[80] = "HH";
    char opciones_minuto[200] = "MM";
    char item[5];
    for(unsigned i = 0; i < 24; i++) {
        snprintf(item, sizeof(item), "\n%02u", i);
        strcat(opciones_hora, item);
    }
    for(unsigned i = 0; i < 60; i++) {
        snprintf(item, sizeof(item), "\n%02u", i);
        strcat(opciones_minuto, item);
    }
    formulario.hora = selector(panel, opciones_hora, 24, 120, 84);
    texto(panel, ":", 118, 130, 14);
    formulario.minuto = selector(panel, opciones_minuto, 136, 120, 84);

    texto(panel, "Método", 248, 94, 290);
    formulario.metodo = selector(panel, "Elegir...\nNFT\nInundación/drenaje\nLluvia", 248, 120, 300);
    texto(panel, "Sistema", 572, 94, 164);
    formulario.destino = selector(panel, "Elegir...\nMódulo 1\nMódulo 2\nAmbos", 572, 120, 164);
    lv_obj_add_event_cb(formulario.metodo, metodo_cambiado, LV_EVENT_VALUE_CHANGED, NULL);

    texto(panel, "Duración", 24, 190, 400);
    lv_obj_t * menos = boton(panel, "−", 24, 216, 50, 44, COLOR_AZUL);
    formulario.duracion = lv_spinbox_create(panel);
    posicion(formulario.duracion, 84, 216, 100, 44);
    lv_spinbox_set_range(formulario.duracion, 0, 9999);
    lv_spinbox_set_digit_format(formulario.duracion, 4, 0);
    lv_spinbox_set_rollover(formulario.duracion, false);
    lv_spinbox_set_cursor_pos(formulario.duracion, 0);
    lv_spinbox_set_step(formulario.duracion, 1);
    lv_obj_set_style_text_font(formulario.duracion, &ui_fuente_es_18, 0);
    lv_obj_set_style_radius(formulario.duracion, 9, 0);
    /* Evitar abrir teclados externos del grupo al tocar el Spinbox. */
    lv_obj_remove_flag(formulario.duracion, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    registrar(formulario.duracion);
    lv_obj_t * mas = boton(panel, "+", 194, 216, 50, 44, COLOR_AZUL);
    lv_obj_add_event_cb(menos, duracion_paso, LV_EVENT_CLICKED, (void *)(uintptr_t)0);
    lv_obj_add_event_cb(mas, duracion_paso, LV_EVENT_CLICKED, (void *)(uintptr_t)1);
    lv_obj_add_event_cb(menos, duracion_paso, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(uintptr_t)0);
    lv_obj_add_event_cb(mas, duracion_paso, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(uintptr_t)1);
    formulario.unidad = selector(panel, "Unidad...\nSegundos\nMinutos", 264, 216, 164);

    texto(panel, "Estado del evento", 480, 190, 255);
    formulario.habilitado = lv_switch_create(panel);
    posicion(formulario.habilitado, 672, 219, 64, 34);
    registrar(formulario.habilitado);
    formulario.texto_habilitado = texto(panel, "Deshabilitado", 480, 227, 180);
    lv_obj_add_event_cb(formulario.habilitado, habilitado_cambiado, LV_EVENT_VALUE_CHANGED, NULL);

    texto(panel, "Días de la semana", 24, 286, 460);
    lv_obj_t * todos = boton(panel, "Todos", 624, 279, 112, 30, COLOR_AZUL);
    lv_obj_add_event_cb(todos, todos_clicked, LV_EVENT_CLICKED, NULL);
    static const char * nombres_dias[] = {"Lun", "Mar", "Mié", "Jue", "Vie", "Sáb", "Dom"};
    for(unsigned i = 0; i < 7; i++) {
        formulario.dias[i] = boton(panel, nombres_dias[i], 24 + (int)i * 102, 316, 94, 44, 0xE2E9EE);
        lv_obj_add_flag(formulario.dias[i], LV_OBJ_FLAG_CHECKABLE);
        lv_obj_set_style_text_color(formulario.dias[i], lv_color_hex(COLOR_TEXTO), 0);
        lv_obj_set_style_bg_color(formulario.dias[i], lv_color_hex(COLOR_AZUL), LV_STATE_CHECKED);
        lv_obj_set_style_text_color(formulario.dias[i], lv_color_white(), LV_STATE_CHECKED);
    }

    formulario.estado = texto(panel, "Los eventos se ejecutan\nsolo en modo automático.", 24, 382, 408);
    lv_obj_set_style_text_font(formulario.estado, &ui_fuente_es_18, 0);
    lv_obj_set_style_text_color(formulario.estado, lv_color_hex(COLOR_GRIS), 0);
    formulario.cancelar = boton(panel, "Cancelar", 456, 390, 132, 44, 0x63717A);
    formulario.guardar = boton(panel, "Guardar", 600, 390, 136, 44, COLOR_VERDE);
    lv_obj_add_event_cb(formulario.cancelar, cancelar_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(formulario.guardar, guardar_clicked, LV_EVENT_CLICKED, NULL);
}

void ui_evento_agregar_clicked(lv_event_t * e)
{
    if(e != NULL && lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ui_evento_abrir(lv_screen_active());
}

uint32_t ui_evento_solicitud_pendiente(void)
{
    return formulario.solicitud;
}

/**
 * Ignora confirmaciones tardias de otra vista/token. Error libera edicion;
 * exito exige ID del central y cierra la vista, notificando una copia confirmada.
 */
void ui_evento_confirmar(uint32_t solicitud, bool exito,
                        uint32_t id_central, const char * mensaje)
{
    if(formulario.fondo == NULL || formulario.cerrando || solicitud == 0 ||
       solicitud != formulario.solicitud) return;
    if(exito && id_central == 0) {
        estado("Falta el ID del central. Esperando confirmación...", true);
        return;
    }
    formulario.solicitud = 0;
    formulario.incierta = false;
    if(!exito) {
        controles_bloquear(false);
        estado(mensaje != NULL && mensaje[0] != '\0' ? mensaje : "El central rechazó el evento.", true);
        return;
    }
    hmi_evento_t confirmado = formulario.borrador;
    confirmado.id = id_central;
    estado("Evento guardado.", false);
    cerrar();
    if(guardado_cb != NULL) guardado_cb(&confirmado, contexto_cb);
}

/**
 * Carga un evento recibido con ID; conserva su identificacion para EDITAR.
 * Adecua la unidad visual sin cambiar la duracion en segundos del modelo.
 */
bool ui_evento_editar(lv_obj_t * parent, const hmi_evento_t * evento)
{
    if(formulario.fondo || !evento || !evento->id || hmi_evento_validar(evento)) return false;
    bool minutos = evento->duracion_s > 9999;
    if(minutos && (evento->duracion_s % 60 || evento->duracion_s / 60 > 9999)) return false;
    ui_evento_abrir(parent); if(!formulario.fondo) return false;
    formulario.editar_id = evento->id;
    lv_label_set_text(formulario.titulo, "Editar evento de riego");
    lv_dropdown_set_selected(formulario.hora, evento->hora + 1);
    lv_dropdown_set_selected(formulario.minuto, evento->minuto + 1);
    lv_dropdown_set_selected(formulario.metodo, evento->metodo);
    metodo_cambiado(NULL);
    lv_dropdown_set_selected(formulario.destino, evento->metodo == HMI_METODO_LLUVIA ? 0 : evento->destino);
    lv_dropdown_set_selected(formulario.unidad, minutos ? 2 : 1);
    lv_spinbox_set_value(formulario.duracion, minutos ? evento->duracion_s / 60 : evento->duracion_s);
    for(unsigned i=0;i<7;i++) {
        if(evento->dias & (1U << i)) lv_obj_add_state(formulario.dias[i],LV_STATE_CHECKED);
        else lv_obj_remove_state(formulario.dias[i],LV_STATE_CHECKED);
    }
    if(evento->habilitado) lv_obj_add_state(formulario.habilitado,LV_STATE_CHECKED);
    else lv_obj_remove_state(formulario.habilitado,LV_STATE_CHECKED);
    habilitado_cambiado(NULL);
    estado("Editar evento: Guardar solicita el cambio al central.", false);
    return true;
}

/**
 * Un timeout no demuestra rechazo: conserva la solicitud y permite cerrar
 * la vista. Cerrar no cancela ni repite lo ya transmitido al central.
 */
void ui_evento_incierta(uint32_t solicitud, const char * mensaje)
{
    if(!formulario.fondo || solicitud != formulario.solicitud || !solicitud) return;
    formulario.incierta = true;
    estado(mensaje, true);
    /* Cerrar la vista no reenvía ni cancela una solicitud ya transmitida. */
    lv_obj_remove_state(formulario.cancelar,LV_STATE_DISABLED);
}

void ui_evento_cerrar_incierta(uint32_t solicitud)
{
    if(formulario.fondo && formulario.incierta && formulario.solicitud == solicitud) cerrar();
}

/**
 * @file ui_formulario_evento.h
 * Contrato del editor propio para LVGL 9.5, fuera de la exportacion SquareLine.
 * Mantiene un unico formulario y callbacks de envio/confirmacion.
 * El token de solicitud pertenece a la vista y no es el ID definitivo del evento.
 * Toda llamada requiere contexto LVGL; las confirmaciones antiguas se ignoran.
 */
#ifndef UI_FORMULARIO_EVENTO_H
#define UI_FORMULARIO_EVENTO_H

#include <lvgl.h>
#include "hmi_evento.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Copiar evento y solicitud a la cola UART sin bloquear.
 * true = solicitud aceptada para enviar; NO significa evento guardado.
 * false = no enviada. Nunca transmitir la estructura binaria sin un contrato.
 */
typedef bool (*ui_evento_enviar_cb_t)(const hmi_evento_t * evento,
                                     uint32_t solicitud, void * contexto);

/* Solo se llama despues de una confirmacion positiva del central con ID.
 * Copiar el evento si se necesita conservarlo despues de este callback.
 */
typedef void (*ui_evento_guardado_cb_t)(const hmi_evento_t * evento, void * contexto);

void ui_evento_configurar(ui_evento_enviar_cb_t enviar,
                         ui_evento_guardado_cb_t guardado, void * contexto);

/* Un solo formulario. parent debe ser la pantalla activa de 800 x 480.
 * Todos los metodos de este modulo requieren el hilo/contexto de LVGL.
 */
void ui_evento_abrir(lv_obj_t * parent);
bool ui_evento_editar(lv_obj_t * parent, const hmi_evento_t * evento);
void ui_evento_incierta(uint32_t solicitud, const char * mensaje);
void ui_evento_cerrar_incierta(uint32_t solicitud);
void ui_evento_agregar_clicked(lv_event_t * e);

/* Devuelve 0 si no hay una solicitud esperando confirmacion. */
uint32_t ui_evento_solicitud_pendiente(void);

/* Ignora respuestas de otra solicitud o de un formulario destruido.
 * exito requiere id_central != 0. mensaje se copia a un Label en el momento.
 * Un timeout de transporte NO prueba rechazo: antes de reintentar hay que
 * consultar/reconciliar la solicitud con el central para evitar duplicados.
 */
void ui_evento_confirmar(uint32_t solicitud, bool exito,
                        uint32_t id_central, const char * mensaje);

#ifdef __cplusplus
}
#endif
#endif

/**
 * @file hmi_app.h
 * API publica del cliente. hmi_app_init se llama despues de ui_init; rx recibe
 * bytes y tick presta servicio frecuente con millis() del mismo contexto.
 * El callback de transporte recibe una linea completa; true significa que
 * se acepto para enviar, no que el central ejecuto o guardo una orden.
 * Los callbacks LVGL y getters no deben llamarse desde interrupciones.
 */
#ifndef HMI_APP_H
#define HMI_APP_H
#include <lvgl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/**
 * Contrato del transporte: texto con longitud explicita, contexto opaco.
 * La implementacion debe copiar/encolar los bytes antes de retornar true.
 */
typedef bool (*hmi_transmitir_t)(const char *, size_t, void *);
void hmi_app_init(hmi_transmitir_t transmitir, void * contexto, uint32_t ahora);
void hmi_app_tick(uint32_t ahora);
void hmi_app_rx(uint8_t byte, uint32_t ahora);
void hmi_app_abrir_evento(lv_event_t * e);
void hmi_app_aplicar_umbral(lv_event_t * e);
void hmi_app_cargar_historial(lv_event_t * e);
void hmi_app_actualizar_historial(lv_event_t * e);
/* Diagnóstico de aplicación, sin escribir sobre UART0. */
bool hmi_app_conectada(void);
uint32_t hmi_app_muestra(void);
unsigned hmi_app_puntos_recibidos(void);
unsigned hmi_app_eventos(void);
unsigned hmi_app_registros(void);
#ifdef __cplusplus
}
#endif
#endif

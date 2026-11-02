/**
 * @file hmi_reloj.h
 * Vista y editor del reloj del central. Los callbacks consultan/solicitan ajuste
 * por el cliente UART; este modulo no tiene acceso fisico al DS3231.
 * estado comunica permisos confirmados; recibir aporta fecha, zona y calidad.
 * Editar campos no cambia el reloj hasta que el central confirme Guardar.
 */
#ifndef HMI_RELOJ_H
#define HMI_RELOJ_H
#include <lvgl.h>
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef bool (*hmi_reloj_leer_t)(void);
typedef bool (*hmi_reloj_guardar_t)(const char *fecha, const char *hora);
void hmi_reloj_init(lv_obj_t *principal, hmi_reloj_leer_t leer, hmi_reloj_guardar_t guardar);
void hmi_reloj_abrir(lv_event_t *e);
void hmi_reloj_estado(bool enlace, bool permite_ajuste, bool orden_pendiente);
bool hmi_reloj_calendario(const char *fecha, const char *hora);
bool hmi_reloj_recibir(const char *fecha, const char *hora, const char *zona, bool valido, uint32_t ahora);
void hmi_reloj_error(const char *mensaje);
void hmi_reloj_confirmado(void);
void hmi_reloj_tick(uint32_t ahora);
#ifdef __cplusplus
}
#endif
#endif

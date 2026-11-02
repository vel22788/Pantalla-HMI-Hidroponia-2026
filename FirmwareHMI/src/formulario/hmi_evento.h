/**
 * @file hmi_evento.h
 * Modelo RAM de evento del editor. Los enums son representacion local: no
 * enviar la estructura binaria por UART ni confundir sus numeros con los
 * codigos del central. hmi_app.c traduce a METODO/DESTINO textuales.
 * ID=0 identifica un alta; un ID positivo permite editar el evento del central.
 * duracion_s siempre son segundos y dias usa lunes en bit 0, domingo en bit 6.
 */
#ifndef HMI_EVENTO_H
#define HMI_EVENTO_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HMI_METODO_SIN_ELEGIR = 0,
    HMI_METODO_NFT = 1,
    HMI_METODO_INUNDACION_DRENAJE = 2,
    HMI_METODO_LLUVIA = 3
} hmi_metodo_t;

typedef enum {
    HMI_DESTINO_VERTICAL = 0,
    HMI_DESTINO_MODULO_1 = 1,
    HMI_DESTINO_MODULO_2 = 2,
    HMI_DESTINO_AMBOS = 3
} hmi_destino_t;

/* Mascara de dias: bit 0 = lunes, ..., bit 6 = domingo.
 * duracion_s siempre expresa segundos, independientemente de la unidad visual.
 * Los numeros de metodo/destino son internos; aun no son un protocolo UART.
 */
typedef struct {
    uint32_t id;             /* 0 al crear; el central asigna el ID definitivo. */
    uint8_t hora;
    uint8_t minuto;
    hmi_metodo_t metodo;
    hmi_destino_t destino;
    uint32_t duracion_s;
    uint8_t dias;
    bool habilitado;
} hmi_evento_t;

/* NULL si el registro tiene una estructura valida; mensaje en otro caso.
 * El central debe validar tambien limites, recursos y solapamientos.
 */
const char * hmi_evento_validar(const hmi_evento_t * evento);

#ifdef __cplusplus
}
#endif
#endif

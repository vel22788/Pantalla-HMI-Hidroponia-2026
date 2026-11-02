/**
 * @file hmi_evento.c
 * Validacion estructural del borrador antes de transmitirlo. Devuelve NULL
 * si es valido o un mensaje para el usuario; no guarda ni ejecuta eventos.
 * No sustituye la validacion de limites, modo y recursos que hace el central.
 */
#include "hmi_evento.h"
#include <stddef.h>

/**
 * NULL significa estructura admisible. Comprueba hora, metodo/destino,
 * duracion positiva y mascara semanal; el maximo ejecutable de duracion
 * lo aplica el central al recibir la solicitud.
 */
const char * hmi_evento_validar(const hmi_evento_t * evento)
{
    if(evento == NULL) return "No se recibió el evento.";
    if(evento->hora > 23 || evento->minuto > 59) return "Elegí la hora y los minutos.";
    if(evento->metodo < HMI_METODO_NFT || evento->metodo > HMI_METODO_LLUVIA)
        return "Elegí un método de riego.";
    if(evento->metodo == HMI_METODO_LLUVIA) {
        if(evento->destino != HMI_DESTINO_VERTICAL) return "Lluvia utiliza el sistema vertical.";
    }
    else if(evento->destino < HMI_DESTINO_MODULO_1 || evento->destino > HMI_DESTINO_AMBOS) {
        return "Elegí el módulo horizontal.";
    }
    if(evento->duracion_s == 0) return "Ingresá una duración mayor que cero.";
    if(evento->dias == 0 || (evento->dias & 0x80U) != 0)
        return "Seleccioná al menos un día.";
    return NULL;
}

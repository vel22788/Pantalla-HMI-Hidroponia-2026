/**
 * @file hmi_config.h
 * Ajustes del cliente de pantalla: baudios, pines, consultas, watchdog,
 * espera de respuesta, caducidad de datos y capacidades de vistas/buffers.
 * Los sufijos MS estan en milisegundos; guardar cambios exige recompilar/cargar.
 * Estos valores no cambian los tiempos del riego ni la velocidad de motores:
 * eso se configura en el ESP32 central. La entrega usa HMI_DEMO_PC=0.
 * El bloque Demo se conserva como opcion de compilacion; no esta activo aqui.
 */
#ifndef HMI_CONFIG_H
#define HMI_CONFIG_H
/* Parámetros del cliente HMI, ajustables; los límites del proceso están en el central. */
#define HMI_BAUD 115200
#define HMI_RX_PIN 44
#define HMI_TX_PIN 43
/**
 * Cadencia de solicitudes DATOS/ESTADO, separada de la adquisicion central.
 * Consultar mas seguido no produce necesariamente muestras nuevas.
 */
#define HMI_CONSULTA_MS 2000U
#define HMI_PING_MS 1000U
/* Consultar el RTC central; el indicador avanza cada segundo entre lecturas. */
/**
 * El indicador avanza entre consultas. Su caducidad limita cuanto tiempo
 * puede presentarse una hora sin una nueva lectura valida del central.
 */
#define HMI_RELOJ_CONSULTA_MS 10000U
#define HMI_RELOJ_ANTIGUO_MS 30000U
#ifndef HMI_DEMO_PC
#define HMI_DEMO_PC 0
#endif
/* Demo manual por PC: seleccionar con Compilar.ps1 -DemoPC. */
#if HMI_DEMO_PC
#define HMI_ENLACE_MS 120000U
#define HMI_RESPUESTA_MS 60000U
#define HMI_DATO_ANTIGUO_MS 300000U
#else
/**
 * Perfil normal: perder respuesta valida vence enlace; vencer una solicitud
 * no demuestra rechazo. Los datos antiguos se ocultan aunque antes fueran OK.
 */
#define HMI_ENLACE_MS 6000U
#define HMI_RESPUESTA_MS 3000U
#define HMI_DATO_ANTIGUO_MS 30000U
#endif
#define HMI_HOLA_MS 1500U
#define HMI_LINEA_MS 1000U
#define HMI_LINEA_MAX 1024U
/**
 * Capacidades de vista/RAM, no capacidad persistente del central.
 * Los arrays de graficas deben conservar HMI_PUNTOS elementos por serie.
 */
#define HMI_PUNTOS 120U
#define HMI_PAGINA 10U
#define HMI_AGENDA_MAX 40U
#define HMI_HISTORIAL_MAX 80U
#endif

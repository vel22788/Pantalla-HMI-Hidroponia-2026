/**
 * @file FirmwareHMI.ino
 * Firmware completo de la pantalla Elecrow DIS08070H V3, ESP32-S3, 800 x 480.
 * Inicializa RGB, tactil GT911, expansor PCA9557, LVGL y el cliente ASCII V2.
 * La pantalla presenta datos/estados y solicita acciones; el central ejecuta
 * sensores, motores, riego, agenda y protecciones. No es el programa central.
 * 
 * Recorrido: UART -> cliente/temporizadores -> LVGL, en un unico loop.
 * La cola de salida evita esperar a transmitir una linea completa. UART0 es
 * exclusiva del protocolo: no imprimir depuracion sobre ese canal.
 * Version de entrega: interfaz con eventos y reloj del central.
 * src/app/hmi_config.h contiene los tiempos del cliente, no los de riego.
 * Los pines RGB/porches corresponden al panel; no son ajustes de velocidad.
 */
/* HMI V2: controlador RGB/GT911 y cliente UART ASCII V2. */
#include <PCA9557.h>
#include <lvgl.h>
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <SPI.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include "src/ui/ui.h"
#include "src/app/hmi_app.h"
#include "src/app/hmi_config.h"
#define TFT_BL 2
/**
 * Base RGB del fabricante: los 16 pines de datos y sincronismos identifican
 * el panel instalado. La construccion configura el objeto; lcd.begin lo inicia.
 */
class LGFX : public lgfx::LGFX_Device
{
public:
  lgfx::Bus_RGB _bus_instance;
  lgfx::Panel_RGB _panel_instance;

  /**
   * @brief Configure the RGB bus and its 800 by 480 panel.
   *
   * The data-pin order and synchronization timing must match the physical
   * panel so the display receives correctly aligned RGB565 frames.
   *
   * @param None.
   * @return A configured LGFX display object.
   * @note Called automatically while the global lcd object is constructed.
   */
  LGFX()
  {
    auto busConfig = _bus_instance.config();
    busConfig.panel = &_panel_instance;
    const int8_t dataPins[16] = {15, 7, 6, 5, 4, 9, 46, 3, 8, 16, 1, 14, 21, 47, 48, 45};
    memcpy(busConfig.pin_data, dataPins, sizeof(dataPins));
    busConfig.pin_henable = 41;
    busConfig.pin_vsync = 40;
    busConfig.pin_hsync = 39;
    busConfig.pin_pclk = 0;
    busConfig.freq_write = 24000000;
    busConfig.hsync_polarity = 0;
    busConfig.hsync_front_porch = 40;
    busConfig.hsync_pulse_width = 48;
    busConfig.hsync_back_porch = 40;
    busConfig.vsync_polarity = 0;
    busConfig.vsync_front_porch = 1;
    busConfig.vsync_pulse_width = 31;
    busConfig.vsync_back_porch = 13;
    busConfig.pclk_active_neg = 1;
    busConfig.de_idle_high = 0;
    busConfig.pclk_idle_high = 0;
    _bus_instance.config(busConfig);

    auto panelConfig = _panel_instance.config();
    panelConfig.memory_width = 800;
    panelConfig.memory_height = 480;
    panelConfig.panel_width = 800;
    panelConfig.panel_height = 480;
    _panel_instance.config(panelConfig);
    _panel_instance.setBus(&_bus_instance);
    setPanel(&_panel_instance);
  }
};

// Owns the RGB bus, panel, and frame buffers used for drawing.
LGFX lcd;


static constexpr uint32_t screenWidth=800, screenHeight=480;
SPIClass& spi=SPI;
#include "touch.h"
PCA9557 Out;
static bool uiReady=false;
/**
 * Buffer circular de bytes por transmitir. Head escribe y Tail retira;
 * no se imprime texto de diagnostico mezclado con el protocolo ASCII.
 */
static uint8_t txQueue[4096];
static unsigned txHead=0, txTail=0, txCount=0;

/**
 * Acepta la linea entera solo si cabe en la cola. false indica que no
 * se encolo; true no es un ACK del central ni una confirmacion de actuacion.
 */
static bool transmitir(const char *text, size_t length, void *)
{
  if(length>sizeof(txQueue)-txCount) return false;
  for(size_t i=0;i<length;i++) { txQueue[txHead]=(uint8_t)text[i]; txHead=(txHead+1)%sizeof(txQueue); }
  txCount+=(unsigned)length; return true;
}
/**
 * Recibe hasta 256 bytes y transmite hasta 256 por vuelta segun espacio UART.
 * Conservar los bytes no enviados permite seguir atendiendo tactil y temporizadores.
 */
static void atenderUART()
{
  unsigned budget=256;
  while(budget-- && Serial0.available()) hmi_app_rx((uint8_t)Serial0.read(),millis());
  int room=Serial0.availableForWrite();
  if(room>0 && txCount) {
    unsigned amount=txCount;
    if(amount>(unsigned)room) amount=(unsigned)room;
    if(amount>sizeof(txQueue)-txTail) amount=sizeof(txQueue)-txTail;
    if(amount>256) amount=256;
    size_t written=Serial0.write(txQueue+txTail,amount);
    txTail=(txTail+written)%sizeof(txQueue); txCount-=(unsigned)written;
  }
}
static void logSilencioso(lv_log_level_t, const char *) {}
/**
 * Entrega a RGB el framebuffer de LVGL en modo FULL y avisa fin de flush.
 * No copiar pixeles uno por uno: los dos buffers proceden del bus/panel.
 */
void my_disp_flush(lv_display_t *display,const lv_area_t *,uint8_t *pixels)
{
  lcd._bus_instance.presentFrameBuffer(pixels);
  lv_display_flush_ready(display);
}
/**
 * Adapta contacto GT911 al estado PR/REL y coordenadas LVGL.
 * Si no hay contacto la lectura queda liberada, sin inventar una posicion.
 */
void my_touchpad_read(lv_indev_t *,lv_indev_data_t *data)
{
  data->state=LV_INDEV_STATE_REL;
  if(touch_has_signal() && touch_touched()) {
    data->state=LV_INDEV_STATE_PR; data->point.x=touch_last_x; data->point.y=touch_last_y;
  }
}
/**
 * Inicializacion unica: PSRAM -> I2C/expansor -> RGB -> LVGL/tactil -> UI ->
 * cliente -> UART0. Si faltan PSRAM/panel/buffers, uiReady sigue false y loop
 * no atiende la UI. La copia no agrega impresiones sobre UART de protocolo.
 */
void setup()
{
  // UART0 queda reservado al protocolo; sin impresiones de depuración.
  esp_log_level_set("*",ESP_LOG_NONE);
  if(!psramFound()) return;
  Wire.begin(19,20);
  Out.reset(); Out.setMode(IO_OUTPUT);
  Out.setState(IO0,IO_LOW); Out.setState(IO1,IO_LOW); delay(20);
  Out.setState(IO0,IO_HIGH); delay(100); Out.setMode(IO1,IO_INPUT);
  if(!lcd.begin()) return;
  delay(200); lv_init(); lv_log_register_print_cb(logSilencioso); lv_tick_set_cb(millis);
  touch_init();
  uint8_t *fb0=(uint8_t*)lcd._bus_instance.getFrameBuffer(0);
  uint8_t *fb1=(uint8_t*)lcd._bus_instance.getFrameBuffer(1);
  if(!fb0 || !fb1) return;
  lv_display_t *display=lv_display_create(screenWidth,screenHeight);
  lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(display,my_disp_flush);
  lv_display_set_buffers(display,fb0,fb1,screenWidth*screenHeight*2U,LV_DISPLAY_RENDER_MODE_FULL);
  lv_indev_t *indev=lv_indev_create(); lv_indev_set_type(indev,LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev,my_touchpad_read);
  pinMode(TFT_BL,OUTPUT); digitalWrite(TFT_BL,HIGH);
  ui_init();
  hmi_app_init(transmitir,nullptr,millis());
  Serial0.setRxBufferSize(4096); Serial0.begin(HMI_BAUD,SERIAL_8N1,HMI_RX_PIN,HMI_TX_PIN);
  uiReady=true;
}
/**
 * Servicio cooperativo de transporte, cliente y LVGL. La pausa de 2 ms
 * no es la cadencia de consultas: esas se comparan en hmi_app_tick.
 */
void loop()
{
  if(uiReady) { atenderUART(); hmi_app_tick(millis()); lv_timer_handler(); }
  delay(2);
}

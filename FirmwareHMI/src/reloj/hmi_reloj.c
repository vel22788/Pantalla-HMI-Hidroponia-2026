/**
 * @file hmi_reloj.c
 * Indicador y editor de fecha/hora local de Guatemala, formato de 24 horas.
 * El calendario central es la referencia; se interpolan segundos con millis()
 * entre respuestas y se invalida la vista cuando faltan enlace o frescura.
 * El contador propio toma 2000 como origen y solo sirve para presentar fecha.
 * Los campos editables son un borrador; Leer los repone y Guardar solicita
 * un ajuste explicito. No hay NTP ni sincronizacion por hora de compilacion.
 */
/* El DS3231 del central conserva la fecha. La HMI solo la presenta y solicita
 * ajustes explícitos. Los segundos locales se interpolan entre consultas. */
#include "hmi_reloj.h"
#include "../app/hmi_config.h"
#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(ui_fuente_es_18);
static struct {
    hmi_reloj_leer_t leer;
    hmi_reloj_guardar_t guardar;
    lv_obj_t *hora, *fecha, *ventana, *actual, *estado, *campos[6], *pasos[12], *guardar_btn, *leer_btn;
    bool enlace, permite, pendiente, valido, rellenar;
    uint32_t segundos, recibido, ahora, dibujado;
} reloj;

static unsigned dias_mes(unsigned y, unsigned m)
{
    static const unsigned dias[]={31,28,31,30,31,30,31,31,30,31,30,31};
    return dias[m-1]+(m==2 && y%4==0);
}
/**
 * Valida fecha AAAA-MM-DD/hora HH:MM:SS entre 2000 y 2099.
 * Se usa la regla bisiesta correspondiente a ese intervalo.
 */
bool hmi_reloj_calendario(const char *fecha, const char *hora)
{
    if(!fecha || !hora || strlen(fecha)!=10 || strlen(hora)!=8 || fecha[4]!='-' || fecha[7]!='-' || hora[2]!=':' || hora[5]!=':') return false;
    for(unsigned i=0;i<10;i++) if(i!=4 && i!=7 && (fecha[i]<'0'||fecha[i]>'9')) return false;
    for(unsigned i=0;i<8;i++) if(i!=2 && i!=5 && (hora[i]<'0'||hora[i]>'9')) return false;
    unsigned y,m,d,h,min,s;
    if(sscanf(fecha,"%u-%u-%u",&y,&m,&d)!=3 || sscanf(hora,"%u:%u:%u",&h,&min,&s)!=3) return false;
    return y>=2000 && y<=2099 && m>=1 && m<=12 && d>=1 && d<=dias_mes(y,m) && h<=23 && min<=59 && s<=59;
}
static uint32_t a_segundos(const char *fecha, const char *hora)
{
    unsigned y,m,d,h,min,s; sscanf(fecha,"%u-%u-%u",&y,&m,&d); sscanf(hora,"%u:%u:%u",&h,&min,&s);
    uint32_t dias=d-1;
    for(unsigned n=2000;n<y;n++) dias+=n%4==0?366:365;
    for(unsigned n=1;n<m;n++) dias+=dias_mes(y,n);
    return dias*86400U+h*3600U+min*60U+s;
}
/**
 * Interpola desde la lectura central solo mientras enlace, calidad y edad
 * sean validos. Devuelve false fuera de ese intervalo; no usa la hora de la PC.
 */
static bool calendario_actual(unsigned *v)
{
    if(!reloj.enlace || !reloj.valido || (uint32_t)(reloj.ahora-reloj.recibido)>HMI_RELOJ_ANTIGUO_MS) return false;
    uint32_t total=reloj.segundos+(uint32_t)(reloj.ahora-reloj.recibido)/1000U, dias=total/86400U;
    unsigned y=2000,m=1;
    while(y<=2099 && dias>=(y%4==0?366U:365U)) { dias-=y%4==0?366U:365U; y++; }
    if(y>2099) return false;
    while(dias>=dias_mes(y,m)) { dias-=dias_mes(y,m); m++; }
    v[0]=dias+1; v[1]=m; v[2]=y; v[3]=total/3600U%24U; v[4]=total/60U%60U; v[5]=total%60U;
    return true;
}
static lv_obj_t *texto(lv_obj_t *padre, const char *s, int x, int y, int ancho)
{
    lv_obj_t *o=lv_label_create(padre); lv_obj_set_pos(o,x,y); lv_obj_set_width(o,ancho);
    lv_obj_set_style_text_font(o,&ui_fuente_es_18,0); lv_label_set_text(o,s); return o;
}
static lv_obj_t *boton(lv_obj_t *padre, const char *s, int x, int y, int ancho, int alto, lv_event_cb_t cb, uintptr_t dato)
{
    lv_obj_t *o=lv_button_create(padre); lv_obj_set_pos(o,x,y); lv_obj_set_size(o,ancho,alto);
    lv_obj_set_style_bg_color(o,lv_color_hex(0x1976D2),0); lv_obj_set_style_pad_all(o,0,0);
    lv_obj_t *t=texto(o,s,0,0,ancho); lv_obj_set_style_text_align(t,LV_TEXT_ALIGN_CENTER,0); lv_obj_center(t);
    lv_obj_add_event_cb(o,cb,LV_EVENT_CLICKED,(void *)dato); return o;
}
static void habilitar(lv_obj_t *o, bool si)
{ if(si) lv_obj_remove_state(o,LV_STATE_DISABLED); else lv_obj_add_state(o,LV_STATE_DISABLED); }
static void controles(void)
{
    if(!reloj.ventana) return;
    habilitar(reloj.guardar_btn,reloj.enlace && reloj.permite && !reloj.pendiente);
    habilitar(reloj.leer_btn,reloj.enlace && !reloj.pendiente);
    for(unsigned i=0;i<6;i++) habilitar(reloj.campos[i],!reloj.pendiente);
    for(unsigned i=0;i<12;i++) habilitar(reloj.pasos[i],!reloj.pendiente);
}
static void rellenar(void)
{
    unsigned v[6]={1,1,2000,0,0,0}; calendario_actual(v);
    for(unsigned i=0;i<6;i++) lv_spinbox_set_value(reloj.campos[i],(int32_t)v[i]);
}
static void presentar(void)
{
    unsigned v[6]; char hora[40],fecha[32],actual[90];
    if(calendario_actual(v)) {
        snprintf(hora,sizeof(hora),"Hora %02u:%02u:%02u",v[3],v[4],v[5]);
        snprintf(fecha,sizeof(fecha),"%02u/%02u/%04u",v[0],v[1],v[2]);
        snprintf(actual,sizeof(actual),"Central: %s  %02u:%02u:%02u",fecha,v[3],v[4],v[5]);
    } else {
        strcpy(hora,"Hora --:--:--"); strcpy(fecha,"Fecha sin datos");
        strcpy(actual,reloj.enlace?"Central: reloj sin datos válidos":"Central: sin conexión");
    }
    lv_label_set_text(reloj.hora,hora); lv_label_set_text(reloj.fecha,fecha);
    if(reloj.ventana) lv_label_set_text(reloj.actual,actual);
}
void hmi_reloj_estado(bool enlace, bool permite_ajuste, bool orden_pendiente)
{
    if(!enlace) reloj.valido=false;
    bool cambio=reloj.enlace!=enlace;
    reloj.enlace=enlace; reloj.permite=permite_ajuste; reloj.pendiente=orden_pendiente;
    if(cambio) presentar();
    controles();
}
/**
 * Solo acepta zona -06:00 y calendario coherente. Una consulta periodica
 * no sobrescribe campos editados: rellenar indica una lectura solicitada
 * explicitamente para reponer el borrador del editor.
 */
bool hmi_reloj_recibir(const char *fecha, const char *hora, const char *zona, bool valido, uint32_t ahora)
{
    if(!zona || strcmp(zona,"-06:00")) return false;
    if(valido ? !hmi_reloj_calendario(fecha,hora) : (!fecha || !hora || strcmp(fecha,"NA") || strcmp(hora,"NA"))) return false;
    reloj.valido=valido; reloj.recibido=reloj.ahora=ahora;
    if(valido) reloj.segundos=a_segundos(fecha,hora);
    if(reloj.ventana && reloj.rellenar) {
        if(valido) rellenar();
        lv_label_set_text(reloj.estado,valido?"Hora recibida. Editá los campos y tocá Guardar.":"Reloj sin ajustar. Ingresá la fecha y hora completas.");
        reloj.rellenar=false;
    }
    presentar(); return true;
}
void hmi_reloj_error(const char *mensaje)
{ if(reloj.ventana) lv_label_set_text(reloj.estado,mensaje); }
/**
 * Repone los campos con la hora confirmada incluso si se cerro/reabrio
 * el editor mientras Guardar seguia pendiente; despues informa exito.
 */
void hmi_reloj_confirmado(void)
{
    if(reloj.ventana) rellenar();
    hmi_reloj_error("Fecha y hora confirmadas por el central.");
}
static void cerrar(lv_event_t *e)
{
    (void)e; lv_obj_delete(reloj.ventana); reloj.ventana=NULL; reloj.rellenar=false;
}
static void paso(lv_event_t *e)
{
    if(reloj.pendiente) return;
    reloj.rellenar=false;
    uintptr_t dato=(uintptr_t)lv_event_get_user_data(e); lv_obj_t *campo=reloj.campos[dato/2];
    lv_spinbox_set_step(campo,1);
    if(dato%2) lv_spinbox_increment(campo); else lv_spinbox_decrement(campo);
    hmi_reloj_error("Edición sin guardar. El reloj del central sigue funcionando.");
}
static void leer(lv_event_t *e)
{
    (void)e;
    if(!reloj.enlace || reloj.pendiente) return;
    reloj.rellenar=true;
    if(reloj.leer()) hmi_reloj_error("Consultando el reloj del central...");
    else { reloj.rellenar=false; hmi_reloj_error("No se pudo consultar el reloj."); }
}
/**
 * Lee el borrador, valida y llama al transporte solo con permiso confirmado
 * y ninguna orden pendiente. Cambiar spinboxes por si solo no escribe el RTC.
 */
static void guardar(lv_event_t *e)
{
    (void)e;
    if(!reloj.enlace || !reloj.permite || reloj.pendiente) return;
    char fecha[32],hora[32];
    snprintf(fecha,sizeof(fecha),"%04ld-%02ld-%02ld",(long)lv_spinbox_get_value(reloj.campos[2]),(long)lv_spinbox_get_value(reloj.campos[1]),(long)lv_spinbox_get_value(reloj.campos[0]));
    snprintf(hora,sizeof(hora),"%02ld:%02ld:%02ld",(long)lv_spinbox_get_value(reloj.campos[3]),(long)lv_spinbox_get_value(reloj.campos[4]),(long)lv_spinbox_get_value(reloj.campos[5]));
    if(!hmi_reloj_calendario(fecha,hora)) { hmi_reloj_error("Fecha inválida. Revisá el día y el mes, incluido febrero."); return; }
    reloj.rellenar=false;
    if(reloj.guardar(fecha,hora)) hmi_reloj_error("Guardando; esperando confirmación del central...");
    else hmi_reloj_error("No se pudo enviar. Revisá conexión, modo y procesos activos.");
}
/**
 * Construye una sola ventana de ajuste y ofrece Leer/Guardar. Los controles
 * se bloquean durante solicitudes pendientes para no mezclar ediciones.
 */
void hmi_reloj_abrir(lv_event_t *e)
{
    (void)e; if(reloj.ventana) return;
    reloj.ventana=lv_obj_create(lv_layer_top()); lv_obj_set_pos(reloj.ventana,0,0); lv_obj_set_size(reloj.ventana,800,480);
    lv_obj_set_style_pad_all(reloj.ventana,0,0); lv_obj_set_style_border_width(reloj.ventana,0,0);
    lv_obj_set_style_bg_color(reloj.ventana,lv_color_hex(0xF1F5F8),0); lv_obj_remove_flag(reloj.ventana,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *t=texto(reloj.ventana,"Fecha y hora",24,16,750); lv_obj_set_style_text_font(t,&lv_font_montserrat_24,0);
    reloj.actual=texto(reloj.ventana,"",24,59,750);
    texto(reloj.ventana,"Hora local de Guatemala (UTC-6). Formato de 24 horas.",24,91,750);
    const char *nombres[]={"Día","Mes","Año","Hora","Minuto","Segundo"};
    const int min[]={1,1,2000,0,0,0},max[]={31,12,2099,23,59,59};
    for(unsigned i=0;i<6;i++) {
        int x=24+(int)(i%3)*256,y=i<3?128:226;
        texto(reloj.ventana,nombres[i],x,y,220);
        reloj.campos[i]=lv_spinbox_create(reloj.ventana); lv_obj_t *c=reloj.campos[i];
        lv_obj_set_pos(c,x+56,y+26); lv_obj_set_size(c,104,46); lv_obj_set_style_text_font(c,&lv_font_montserrat_24,0);
        lv_spinbox_set_range(c,min[i],max[i]); lv_spinbox_set_digit_format(c,i==2?4:2,0);
        lv_spinbox_set_rollover(c,false); lv_spinbox_set_step(c,1); lv_spinbox_set_cursor_pos(c,0);
        lv_obj_remove_flag(c,LV_OBJ_FLAG_CLICK_FOCUSABLE);
        for(unsigned k=0;k<2;k++) {
            uintptr_t dato=i*2+k;
            reloj.pasos[dato]=boton(reloj.ventana,k?"+":"-",x+(k?168:0),y+26,48,46,paso,dato);
            lv_obj_add_event_cb(reloj.pasos[dato],paso,LV_EVENT_LONG_PRESSED_REPEAT,(void *)dato);
        }
    }
    reloj.estado=texto(reloj.ventana,"Ajustá los campos y tocá Guardar hora para enviar el cambio.",24,334,750);
    lv_label_set_long_mode(reloj.estado,LV_LABEL_LONG_WRAP);
    texto(reloj.ventana,"Para guardar: modo Manual, riegos y movimientos detenidos.",24,378,750);
    boton(reloj.ventana,"Volver",24,412,180,48,cerrar,0);
    reloj.leer_btn=boton(reloj.ventana,"Leer del central",242,412,230,48,leer,0);
    reloj.guardar_btn=boton(reloj.ventana,"Guardar hora",584,412,192,48,guardar,0);
    rellenar(); presentar(); controles();
    if(reloj.enlace) leer(NULL);
}
void hmi_reloj_init(lv_obj_t *principal, hmi_reloj_leer_t consultar, hmi_reloj_guardar_t ajustar)
{
    memset(&reloj,0,sizeof(reloj)); reloj.leer=consultar; reloj.guardar=ajustar;
    reloj.fecha=texto(principal,"Fecha sin datos",14,1,200);
    lv_obj_set_style_text_color(reloj.fecha,lv_color_hex(0x455A64),0);
    lv_obj_t *b=boton(principal,"Hora --:--:--",554,0,240,24,hmi_reloj_abrir,0);
    reloj.hora=lv_obj_get_child(b,0);
}
/**
 * Actualiza la presentacion cada segundo. La consulta cada diez segundos
 * la programa hmi_app_tick; ambas cadencias son independientes.
 */
void hmi_reloj_tick(uint32_t ahora)
{
    reloj.ahora=ahora;
    if((uint32_t)(ahora-reloj.dibujado)>=1000U) { reloj.dibujado=ahora; presentar(); }
}

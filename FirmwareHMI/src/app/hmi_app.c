/**
 * @file hmi_app.c
 * Aplicacion de la HMI: cliente ASCII V2, vistas, controles, agenda y graficas.
 * Todo se atiende en el contexto loop/LVGL; no llamar callbacks desde una ISR.
 * El central es la autoridad de estado y persistencia. Pulsar ON o Guardar solo
 * solicita una orden; OK/ACEPTADO y las consultas posteriores determinan resultado.
 * 
 * Sesion: HOLA VERSION=2 -> BOOT/PROX_ID -> consultas con ID -> respuestas.
 * Cada solicitud pendiente tiene tipo y vencimiento. Si una modificacion queda
 * sin confirmar se consulta ORDEN VER, sin repetir la actuacion automaticamente.
 * Un BOOT nuevo invalida vistas y negocia otra sesion, aunque sigan llegando PONG.
 * 
 * Datos: calidad y edad por magnitud; pH provisional se muestra con ~. Graficas
 * guardan una ventana RAM y no agregan de nuevo una muestra con mismo BOOT/numero.
 * Agenda/historial llegan paginados; no se inventan localmente ni se ejecutan aqui.
 * El reloj usa el DS3231 central por RELOJ VER/AJUSTAR, no la fecha de los sensores.
 */
/* Cliente HMI del catálogo ASCII V2. Un solo contexto: loop/LVGL.
 * El central conserva sensores, RTC, protecciones y ejecución del proceso. */
#include "hmi_app.h"
#include "hmi_config.h"
#include "../ui/ui.h"
#include "../formulario/ui_formulario_evento.h"
#include "../reloj/hmi_reloj.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <ctype.h>

LV_FONT_DECLARE(ui_fuente_es_18);
typedef enum { Q_NONE, Q_OK, Q_ERROR, Q_PROVISIONAL } quality_t;
typedef enum { K_PING, K_DATA, K_STATE, K_THRESHOLD, K_AGENDA, K_HISTORY,
    K_CURVES, K_CLOCK, K_MODE, K_WATER, K_MOVE, K_STOP, K_SET_THRESHOLD,
    K_EVENT_SAVE, K_ACK, K_SET_CLOCK, K_EVENT_CHANGE, K_RESULT } kind_t;
/**
 * Peticion pendiente: ID de protocolo, tipo esperado, tiempo de envio y
 * token del formulario cuando aplica. No es una orden ya confirmada.
 */
typedef struct { uint32_t id, when, aux, token; kind_t kind; int value;
    char target[12], method[12]; bool used; } pending_t;
typedef struct { char boot[17]; uint32_t num, received, age[5];
    double value[5]; quality_t quality[5]; bool age_na[5]; char ts[5][26]; } sample_t;
typedef struct { char id[49], date[26], code[49], detail[160]; } record_t;
typedef struct { uint32_t id, rev, total, got; bool started, first;
    char snap[65], cursor[65]; } transfer_t;
typedef struct { char buf[HMI_LINEA_MAX + 1], *word[4], *key[48], *val[48];
    unsigned words, fields; } line_t;
/**
 * Estado unico de la aplicacion: sesion, peticiones, datos, borradores,
 * snapshots y widgets. El central conserva las decisiones de proceso.
 */
static struct {
    hmi_transmitir_t tx; void * context; uint32_t now, next_id, last_rx,
        hello_at, data_at, state_at, ping_at, rx_at, clock_at;
    bool linked, hello_wait, state_valid, cfg_valid, dirty, syncing, have_data;
    uint32_t rev, cfg_rev; int confirmed;
    char boot[17], uncertain_boot[17], mode[8], level[16], water[5][16], move[2][16], rule[20];
    pending_t pending[12], uncertain;
    sample_t data;
    char input[HMI_LINEA_MAX + 1]; unsigned input_len; bool discard;
    transfer_t agenda_x, history_x, curves_x;
    hmi_evento_t agenda[HMI_AGENDA_MAX], agenda_page[HMI_PAGINA];
    record_t history[HMI_HISTORIAL_MAX], history_page[HMI_PAGINA];
    sample_t curves_page[HMI_PAGINA];
    unsigned agenda_n, history_n, curve_n, curve_pages;
    uint32_t labels_at;
    bool rx_cr;
    bool agenda_reload;
    lv_obj_t * popup;
    uint32_t delete_id;
    char curve_boot[17]; uint32_t curve_num;
    lv_chart_series_t * series[4]; int32_t points[4][HMI_PUNTOS];
    lv_obj_t * status, *manual_status, *threshold_status, *agenda_status,
        *history_status, *curve_status, *agenda_more, *history_more;
} app;

static void restart_link(bool central_restarted);
static void clear_session_views(void);
static bool clock_request(void);

static bool eq(const char *a, const char *b) { return a && b && strcmp(a,b)==0; }
static void copy(char *out, size_t size, const char *in) { snprintf(out,size,"%s",in?in:""); }
static bool uint_value(const char *s, uint32_t *n)
{
    if(!s || !*s) return false;
    uint32_t value=0;
    for(;*s;s++) { if(*s<'0'||*s>'9') return false; unsigned d=(unsigned)(*s-'0');
        if(value>(UINT32_MAX-d)/10U) return false;
        value=value*10U+d;
    }
    *n=value; return true;
}
static bool ascii_token(const char *s, size_t max)
{
    if(!s || !*s || strlen(s)>max) return false;
    for(;*s;s++) if((unsigned char)*s<33 || (unsigned char)*s>126 || *s=='=') return false;
    return true;
}
static bool timestamp(const char *s)
{
    if(eq(s,"NA")) return true;
    if(!s || strlen(s)!=25 || strcmp(s+19,"-06:00")) return false;
    const unsigned positions[]={4,7,10,13,16}; const char marks[]={'-','-','T',':',':'};
    for(unsigned i=0;i<5;i++) if(s[positions[i]]!=marks[i]) return false;
    for(unsigned i=0;i<19;i++) { bool mark=false; for(unsigned j=0;j<5;j++) mark|=i==positions[j];
        if(!mark && !isdigit((unsigned char)s[i])) return false; }
    int y,m,d,h,min,sec;
    if(sscanf(s,"%4d-%2d-%2dT%2d:%2d:%2d",&y,&m,&d,&h,&min,&sec)!=6) return false;
    const int days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    if(y<1||m<1||m>12||h>23||min>59||sec>59||d<1) return false;
    int maximum=days[m-1]+(m==2 && y%4==0 && (y%100!=0||y%400==0));
    return d<=maximum;
}
static bool one_of(const char *s, const char *list)
{
    if(!s) return false;
    size_t n=strlen(s);
    while(*list) { const char *end=strchr(list,'|'); size_t len=end?(size_t)(end-list):strlen(list);
        if(n==len && !strncmp(s,list,len)) return true;
        if(!end) break;
        list=end+1;
    }
    return false;
}
/**
 * Analiza una linea completa en palabras y campos CLAVE=VALOR, con limites
 * y rechazo de claves repetidas. No se actua con una respuesta parcial.
 */
static bool parse(line_t *l, const char *text)
{
    memset(l,0,sizeof(*l)); copy(l->buf,sizeof(l->buf),text); char *p=l->buf;
    while(*p) {
        while(*p==' ') p++;
        if(!*p) break;
        char *start=p;
        while(*p && *p!=' ') p++;
        if(*p) *p++=0;
        char *equal=strchr(start,'=');
        if(equal) {
            *equal=0; if(!*start || !equal[1] || strchr(equal+1,'=') || l->fields==48) return false;
            for(char *c=start;*c;c++) if(!(*c=='_' || (*c>='A'&&*c<='Z') || isdigit((unsigned char)*c))) return false;
            for(unsigned i=0;i<l->fields;i++) if(eq(l->key[i],start)) return false;
            l->key[l->fields]=start; l->val[l->fields++]=equal+1;
        } else { if(l->words==4) return false; l->word[l->words++]=start; }
    }
    return l->words!=0;
}
static const char *field(const line_t *l,const char *key)
{ for(unsigned i=0;i<l->fields;i++) if(eq(l->key[i],key)) return l->val[i]; return NULL; }
static bool number(const line_t *l,const char *key,uint32_t *n) { return uint_value(field(l,key),n); }
static bool fields_allowed(const line_t *l, const char *keys)
{ for(unsigned i=0;i<l->fields;i++) if(!one_of(l->key[i],keys)) return false; return true; }
static bool mutating(kind_t k) { return k>=K_MODE && k<=K_EVENT_CHANGE; }
static pending_t *find(uint32_t id)
{ for(unsigned i=0;i<12;i++) if(app.pending[i].used && app.pending[i].id==id) return &app.pending[i]; return NULL; }
static bool busy_kind(kind_t k)
{ for(unsigned i=0;i<12;i++) if(app.pending[i].used && app.pending[i].kind==k) return true; return false; }
/**
 * Bloquea nuevas modificaciones mientras una anterior esta pendiente o
 * incierta. Parar tiene tratamiento prioritario para poder solicitar cancelacion.
 */
static bool busy_mutation(void)
{
    if(app.uncertain.used) return true;
    for(unsigned i=0;i<12;i++) {
        if(app.pending[i].used && mutating(app.pending[i].kind) && app.pending[i].kind!=K_STOP) return true;
    }
    return false;
}
static void message(const char *text)
{ if(app.status) lv_label_set_text(app.status,text); if(app.manual_status) lv_label_set_text(app.manual_status,text); }
static void enabled(lv_obj_t *o, bool yes)
{ if(yes) lv_obj_remove_state(o,LV_STATE_DISABLED); else lv_obj_add_state(o,LV_STATE_DISABLED); }
static unsigned module(void) { return lv_obj_has_state(ui_swModuloHorizontal,LV_STATE_CHECKED)?1U:0U; }
static bool active_water(unsigned index)
{ return app.state_valid && one_of(app.water[index],"INICIANDO|EN_MARCHA|DETENIENDO|FALLO"); }
static const char *water_text(const char *s)
{
    if(eq(s,"INACTIVO")) return "Inactivo";
    if(eq(s,"EN_MARCHA")) return "Activo";
    if(eq(s,"INICIANDO")) return "Iniciando";
    if(eq(s,"DETENIENDO")) return "Deteniendo";
    if(eq(s,"FALLO")) return "Fallo";
    return "Sin datos";
}
/**
 * Habilita widgets segun enlace y estado confirmado, no segun una pulsacion
 * optimista. Actualiza tambien permisos del editor de reloj; el central vuelve
 * a validar la orden aunque la UI ya la haya permitido.
 */
static void controls(void)
{
    unsigned m=module(); bool manual=app.linked && app.state_valid && eq(app.mode,"MANUAL");
    if(app.state_valid) {
        if(eq(app.mode,"MANUAL")) lv_obj_add_state(ui_swModoOperacion,LV_STATE_CHECKED);
        else lv_obj_remove_state(ui_swModoOperacion,LV_STATE_CHECKED);
    } else lv_obj_remove_state(ui_swModoOperacion,LV_STATE_CHECKED);
    enabled(ui_swModoOperacion,app.linked && app.state_valid && !busy_mutation());
    lv_obj_t *buttons[]={ui_btnSubir,ui_btnBajar,ui_btnInclinar,ui_btnNivelar};
    for(unsigned i=0;i<4;i++) enabled(buttons[i],manual && !busy_mutation());
    enabled(ui_btnDetenerMovimiento,app.linked);
    enabled(ui_btnNFT,app.linked && (active_water(m)|| (manual&&!busy_mutation())));
    enabled(ui_btnInundacionDrenaje,app.linked && (active_water(m+2)|| (manual&&!busy_mutation())));
    enabled(ui_btnLluvia,app.linked && (active_water(4)|| (manual&&!busy_mutation())));
    lv_label_set_text(ui_lblEstadoNFT,app.state_valid?water_text(app.water[m]):"Sin datos");
    lv_label_set_text(ui_lblEstadoInundacionDrenaje,app.state_valid?water_text(app.water[m+2]):"Sin datos");
    lv_label_set_text(ui_lblEstadoLluvia,app.state_valid?water_text(app.water[4]):"Sin datos");
    lv_label_set_text(ui_lblNivelPrincipal,!app.state_valid?"Sin datos":eq(app.level,"SEGURO")?"Seguro":
                      eq(app.level,"BAJO")?"Bajo":"Desconocido");
    enabled(ui_btnAplicarUmbral,app.linked&&app.cfg_valid&&app.dirty&&!busy_mutation());
    enabled(ui_btnAgregarEvento,app.linked&&!busy_mutation()
#if defined(HMI_SIN_EVENTOS) && HMI_SIN_EVENTOS
      && false
#endif
    );
    enabled(ui_btnActualizarHistorial,app.linked&&!busy_kind(K_HISTORY));
    enabled(app.agenda_more,app.linked && !busy_kind(K_AGENDA) && app.agenda_n<HMI_AGENDA_MAX &&
            app.agenda_x.cursor[0] && !eq(app.agenda_x.cursor,"FIN"));
    enabled(app.history_more,app.linked && !busy_kind(K_HISTORY) && app.history_n<HMI_HISTORIAL_MAX &&
            app.history_x.cursor[0] && !eq(app.history_x.cursor,"FIN"));
    bool stopped=manual && eq(app.move[0],"DETENIDO") && eq(app.move[1],"DETENIDO");
    for(unsigned i=0;i<5;i++) stopped=stopped && eq(app.water[i],"INACTIVO");
    hmi_reloj_estado(app.linked,stopped,busy_mutation());
}
/**
 * Asigna ID y registra pendiente solo si el transporte acepta la linea.
 * Reserva dos huecos para paradas prioritarias. Devuelve NULL si no pudo enviar;
 * un puntero valido significa peticion pendiente, no operacion completada.
 */
static pending_t *request(const char *command,kind_t kind,uint32_t aux,uint32_t token,int value)
{
    if(!app.linked || !app.next_id || app.next_id==UINT32_MAX) return NULL;
    bool priority=kind==K_STOP || (kind==K_WATER && value==0);
    if(mutating(kind)&&!priority&&busy_mutation()) return NULL;
    unsigned limit=priority?12:10; pending_t *p=NULL;
    for(unsigned i=0;i<limit;i++) if(!app.pending[i].used) { p=&app.pending[i]; break; }
    if(!p) return NULL;
    char text[512]; int n=snprintf(text,sizeof(text),"%s ID=%lu\n",command,(unsigned long)app.next_id);
    if(n<0 || (size_t)n>=sizeof(text) || !app.tx(text,(size_t)n,app.context)) return NULL;
    memset(p,0,sizeof(*p)); p->used=true; p->id=app.next_id++; p->when=app.now;
    p->kind=kind; p->aux=aux; p->token=token; p->value=value; controls(); return p;
}
static void state_request(void)
{ if(!busy_kind(K_STATE)) request("ESTADO VER",K_STATE,0,0,0); }
static void threshold_request(void)
{ if(!busy_kind(K_THRESHOLD)) request("UMBRAL VER",K_THRESHOLD,0,0,0); }
static bool clock_request(void)
{
    if(busy_kind(K_CLOCK)) return true;
    if(!request("RELOJ VER",K_CLOCK,0,0,0)) return false;
    app.clock_at=app.now; return true;
}
/**
 * Solicita RELOJ AJUSTAR solo con MANUAL y todos los procesos detenidos.
 * Conserva fecha/hora esperadas para validar la confirmacion del mismo cambio.
 */
static bool clock_save(const char *date,const char *time)
{
    if(!app.state_valid || !eq(app.mode,"MANUAL") || !hmi_reloj_calendario(date,time)) return false;
    for(unsigned i=0;i<2;i++) if(!eq(app.move[i],"DETENIDO")) return false;
    for(unsigned i=0;i<5;i++) if(!eq(app.water[i],"INACTIVO")) return false;
    char command[100]; snprintf(command,sizeof(command),"RELOJ AJUSTAR FECHA=%s HORA=%s",date,time);
    pending_t *p=request(command,K_SET_CLOCK,0,0,0); if(!p) return false;
    copy(p->target,sizeof(p->target),date); copy(p->method,sizeof(p->method),time); return true;
}
/**
 * Solicita una pagina con cursor y limite. La vista no cambia hasta que
 * INICIO/filas/FIN formen una pagina valida del mismo ID/snapshot.
 */
static void page_request(kind_t kind,bool first)
{
    if(busy_kind(kind)) return;
    transfer_t *x=kind==K_AGENDA?&app.agenda_x:kind==K_HISTORY?&app.history_x:&app.curves_x;
    const char *cursor=first?"INICIO":x->cursor; if(!*cursor || eq(cursor,"FIN")) return;
    char command[160]; snprintf(command,sizeof(command),"%s VER CURSOR=%s LIMITE=%u",
        kind==K_AGENDA?"AGENDA":kind==K_HISTORY?"HISTORIAL":"CURVAS",cursor,HMI_PAGINA);
    pending_t *p=request(command,kind,0,0,0); if(!p) return;
    x->id=p->id; x->got=0; x->started=false; x->first=first;
    if(first) { x->snap[0]=0; if(kind==K_CURVES) app.curve_pages=0; }
    if(kind==K_AGENDA) lv_label_set_text(app.agenda_status,"Consultando agenda...");
    if(kind==K_HISTORY) lv_label_set_text(app.history_status,"Consultando historial...");
    if(kind==K_CURVES) lv_label_set_text(app.curve_status,"Consultando mediciones anteriores...");
}
/**
 * Valida BOOT, numero, cinco calidades, valores, fechas y edades. EC ya viene
 * en mS/cm del central. PROVISIONAL conserva valor; ERROR/SIN_DATOS requieren NA.
 * Las muestras historicas no traen edad actual y usan NUM en lugar de MUESTRA.
 */
static bool sample_parse(const line_t *l,sample_t *s,bool historical)
{
    const char *keys="ID|BOOT|MUESTRA|PH|PH_Q|PH_TS|PH_MS|CE|CE_Q|CE_TS|CE_MS|TAMB|TAMB_Q|TAMB_TS|TAMB_MS|HAMB|HAMB_Q|HAMB_TS|HAMB_MS|TSOL|TSOL_Q|TSOL_TS|TSOL_MS";
    if(historical) keys="ID|BOOT|NUM|PH|PH_Q|PH_TS|CE|CE_Q|CE_TS|TAMB|TAMB_Q|TAMB_TS|HAMB|HAMB_Q|HAMB_TS|TSOL|TSOL_Q|TSOL_TS";
    if(!fields_allowed(l,keys)||!ascii_token(field(l,"BOOT"),16)) return false;
    memset(s,0,sizeof(*s)); copy(s->boot,sizeof(s->boot),field(l,"BOOT"));
    if(!number(l,historical?"NUM":"MUESTRA",&s->num) || (historical&&!s->num)) return false;
    const char *names[]={"PH","CE","TAMB","HAMB","TSOL"};
    for(unsigned i=0;i<5;i++) {
        char key[20]; snprintf(key,sizeof(key),"%s_Q",names[i]); const char *q=field(l,key);
        if(!one_of(q,"OK|ERROR|SIN_DATOS|PROVISIONAL")) return false;
        s->quality[i]=eq(q,"OK")?Q_OK:eq(q,"PROVISIONAL")?Q_PROVISIONAL:eq(q,"ERROR")?Q_ERROR:Q_NONE;
        const char *v=field(l,names[i]); if(!v) return false;
        if((s->quality[i]==Q_OK || s->quality[i]==Q_PROVISIONAL)) {
            char *end; double value=strtod(v,&end); if(end==v||*end||!isfinite(value)||fabs(value)>1000000) return false;
            s->value[i]=value;
        } else if(!eq(v,"NA")) return false;
        snprintf(key,sizeof(key),"%s_TS",names[i]); const char *ts=field(l,key);
        if(!timestamp(ts) || (s->quality[i]==Q_NONE&&!eq(ts,"NA"))) return false;
        copy(s->ts[i],sizeof(s->ts[i]),ts);
        if(!historical) {
            snprintf(key,sizeof(key),"%s_MS",names[i]); const char *ms=field(l,key);
            s->age_na[i]=eq(ms,"NA"); if(!s->age_na[i]&&!uint_value(ms,&s->age[i])) return false;
            if(s->quality[i]==Q_NONE&&!s->age_na[i]) return false;
            if(s->quality[i]!=Q_NONE&&s->age_na[i]) return false;
        }
        if(s->num==0 && s->quality[i]!=Q_NONE) return false;
    }
    s->received=app.now; return true;
}
static lv_obj_t *charts(unsigned i)
{ lv_obj_t *a[]={ui_chartPH,ui_chartCE,ui_chartTemperaturaAmbiente,ui_chartHumedadAmbiente}; return a[i]; }
/**
 * Reinicia buffers permanentes con LV_CHART_POINT_NONE. Un hueco de grafica
 * significa ausencia de dato, no un valor cero.
 */
static void curve_clear(void)
{
    for(unsigned i=0;i<4;i++) { for(unsigned j=0;j<HMI_PUNTOS;j++) app.points[i][j]=LV_CHART_POINT_NONE;
        lv_chart_set_x_start_point(charts(i),app.series[i],0); lv_chart_refresh(charts(i)); }
    app.curve_n=0; app.curve_num=0; app.curve_boot[0]=0;
}
/**
 * Deduplica por BOOT/numero e inserta hueco ante salto de muestra/reinicio.
 * Escala pH/EC x100 y ambiente/humedad x10 para representar decimales
 * en enteros de LVGL; no son umbrales agronomicos.
 */
static void curve_add(const sample_t *s)
{
    if(!s->num) return;
    bool same=eq(app.curve_boot,s->boot);
    if(same && s->num<=app.curve_num) return;
    if(app.curve_num && (!same || s->num>app.curve_num+1))
        for(unsigned i=0;i<4;i++) lv_chart_set_next_value(charts(i),app.series[i],LV_CHART_POINT_NONE);
    const int factor[]={100,100,10,10};
    for(unsigned i=0;i<4;i++) lv_chart_set_next_value(charts(i),app.series[i],(s->quality[i]==Q_OK || s->quality[i]==Q_PROVISIONAL)?
        (int32_t)lround(s->value[i]*factor[i]):LV_CHART_POINT_NONE);
    copy(app.curve_boot,sizeof(app.curve_boot),s->boot); app.curve_num=s->num; app.curve_n++;
}
/**
 * Presenta -- cuando el dato falta, falla, envejece o no hay enlace.
 * El pH provisional lleva ~. La edad recibida sigue aumentando localmente;
 * una respuesta repetida no debe rejuvenecer una medicion.
 */
static void values(void)
{
    lv_obj_t *principal[]={ui_lblPH,ui_lblCE,ui_lblTemperaturaAmbiente,ui_lblHumedad};
    lv_obj_t *variables[]={ui_lblPHVariables,ui_lblCEVariables,ui_lblTemperaturaAmbienteVariables,ui_lblHumedadVariables};
    for(unsigned i=0;i<4;i++) {
        char text[32]="--"; uint64_t age=(uint64_t)app.data.age[i]+(uint32_t)(app.now-app.data.received);
        if(app.linked && app.have_data && (app.data.quality[i]==Q_OK || app.data.quality[i]==Q_PROVISIONAL) && age<=HMI_DATO_ANTIGUO_MS)
            snprintf(text,sizeof(text),app.data.quality[i]==Q_PROVISIONAL?"~%.2f":i<2?"%.2f":"%.1f",app.data.value[i]);
        if(!eq(lv_label_get_text(principal[i]),text)) lv_label_set_text(principal[i],text);
        if(!eq(lv_label_get_text(variables[i]),text)) lv_label_set_text(variables[i],text);
    }
    if(app.have_data) {
        uint32_t elapsed=(uint32_t)(app.now-app.data.received);
        for(unsigned i=0;i<5;i++) if(!app.data.age_na[i]) {
            uint64_t age=(uint64_t)app.data.age[i]+elapsed;
            app.data.age[i]=age>UINT32_MAX?UINT32_MAX:(uint32_t)age;
        }
        app.data.received=app.now;
    }
}
/**
 * Actualiza el umbral confirmado por revision. Conserva el borrador del
 * usuario si hay cambios sin aplicar; consultar no sobrescribe su edicion.
 */
static void set_threshold(int value,uint32_t rev,bool persistent)
{
    if(app.cfg_valid && rev<app.cfg_rev) return;
    app.confirmed=value; app.cfg_rev=rev; app.cfg_valid=true;
    if(!app.dirty) { app.syncing=true; lv_spinbox_set_value(ui_spnUmbralHumedad,value);
        lv_slider_set_value(ui_sldUmbralHumedad,value,LV_ANIM_OFF); app.syncing=false; }
    lv_label_set_text(app.threshold_status,app.dirty?"Cambios sin aplicar":persistent?"Umbral confirmado":"Umbral confirmado (volátil)");
    controls();
}
static bool days_parse(const char *text,uint8_t *days)
{
    if(!text || strlen(text)>27) return false;
    char buf[32]; copy(buf,sizeof(buf),text);
    const char *names[]={"LUN","MAR","MIE","JUE","VIE","SAB","DOM"};
    *days=0; int last=-1; char *p=buf;
    while(*p) { char *end=strchr(p,','); if(end) *end=0; int index=-1;
        for(int i=0;i<7;i++) if(eq(p,names[i])) index=i;
        if(index<=last) return false;
        *days|=(uint8_t)(1U<<index); last=index;
        if(!end) break;
        p=end+1;
        if(!*p) return false;
    }
    return *days!=0;
}
static void days_text(uint8_t days,char *out,size_t size)
{
    const char *names[]={"LUN","MAR","MIE","JUE","VIE","SAB","DOM"}; out[0]=0;
    for(unsigned i=0;i<7;i++) if(days&(1U<<i)) { size_t used=strlen(out);
        snprintf(out+used,size-used,"%s%s",used?",":"",names[i]); }
}
static const char *method_name(hmi_metodo_t method)
{ return method==HMI_METODO_NFT?"NFT":method==HMI_METODO_INUNDACION_DRENAJE?"INUNDACION":"LLUVIA"; }
static const char *target_name(hmi_destino_t target)
{ return target==HMI_DESTINO_MODULO_1?"M1":target==HMI_DESTINO_MODULO_2?"M2":target==HMI_DESTINO_AMBOS?"AMBOS":"VERTICAL"; }
/**
 * Traduce metodo/destino textuales y dias a su modelo local. Los enums
 * de la HMI no se transmiten como estructura binaria al central.
 */
static bool event_parse(const line_t *l,hmi_evento_t *e)
{
    if(!fields_allowed(l,"ID|EVENTO_ID|HORA|METODO|DESTINO|DURACION_S|DIAS|HABILITADO")) return false;
    memset(e,0,sizeof(*e)); uint32_t enabled_value;
    if(!number(l,"EVENTO_ID",&e->id)||!e->id||!number(l,"DURACION_S",&e->duracion_s)||
       !number(l,"HABILITADO",&enabled_value)||enabled_value>1||!days_parse(field(l,"DIAS"),&e->dias)) return false;
    const char *time=field(l,"HORA"); if(!time||strlen(time)!=5||time[2]!=':'||
        !isdigit((unsigned char)time[0])||!isdigit((unsigned char)time[1])||
        !isdigit((unsigned char)time[3])||!isdigit((unsigned char)time[4])) return false;
    e->hora=(uint8_t)((time[0]-'0')*10+time[1]-'0'); e->minuto=(uint8_t)((time[3]-'0')*10+time[4]-'0');
    const char *m=field(l,"METODO"), *d=field(l,"DESTINO");
    if(!one_of(m,"NFT|INUNDACION|LLUVIA")||!one_of(d,"M1|M2|AMBOS|VERTICAL")) return false;
    e->metodo=eq(m,"NFT")?HMI_METODO_NFT:eq(m,"INUNDACION")?HMI_METODO_INUNDACION_DRENAJE:HMI_METODO_LLUVIA;
    e->destino=eq(d,"M1")?HMI_DESTINO_MODULO_1:eq(d,"M2")?HMI_DESTINO_MODULO_2:eq(d,"AMBOS")?HMI_DESTINO_AMBOS:HMI_DESTINO_VERTICAL;
    e->habilitado=enabled_value!=0; return hmi_evento_validar(e)==NULL;
}
static lv_obj_t *label(lv_obj_t *parent,const char *text,int x,int y,int width)
{
    lv_obj_t *o=lv_label_create(parent); lv_label_set_text(o,text); lv_obj_set_pos(o,x,y); lv_obj_set_width(o,width);
    lv_obj_set_style_text_font(o,&ui_fuente_es_18,0); lv_label_set_long_mode(o,LV_LABEL_LONG_DOT); return o;
}
static lv_obj_t *button(lv_obj_t *parent,const char *text,int x,int y,int w,int h,lv_event_cb_t cb,uintptr_t data)
{
    lv_obj_t *o=lv_button_create(parent); lv_obj_set_pos(o,x,y); lv_obj_set_size(o,w,h);
    lv_obj_t *t=lv_label_create(o); lv_label_set_text(t,text); lv_obj_center(t);
    lv_obj_set_style_text_font(t,&ui_fuente_es_18,0);
    lv_obj_add_event_cb(o,cb,LV_EVENT_CLICKED,(void*)data); return o;
}
static void event_action(lv_event_t *e);
static void agenda_render(void)
{
    lv_obj_clean(ui_conteventrows); lv_obj_set_flex_flow(ui_conteventrows,LV_FLEX_FLOW_COLUMN);
    lv_obj_set_align(ui_conteventrows,LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(ui_conteventrows,0,0);
    lv_obj_set_style_bg_opa(ui_conteventrows,LV_OPA_TRANSP,0);
    for(unsigned i=0;i<app.agenda_n;i++) {
        hmi_evento_t *ev=&app.agenda[i]; lv_obj_t *row=lv_obj_create(ui_conteventrows);
        lv_obj_set_size(row,740,48); lv_obj_set_style_pad_all(row,0,0); lv_obj_remove_flag(row,LV_OBJ_FLAG_SCROLLABLE);
        char time[12],duration[24],days[32]; snprintf(time,sizeof(time),"%02u:%02u",ev->hora,ev->minuto);
        snprintf(duration,sizeof(duration),"%lu s",(unsigned long)ev->duracion_s); days_text(ev->dias,days,sizeof(days));
        label(row,time,3,12,83); char method[48]; snprintf(method,sizeof(method),"%s %s",method_name(ev->metodo),target_name(ev->destino));
        label(row,method,87,12,178); label(row,duration,267,12,95); label(row,days,365,12,160);
        button(row,ev->habilitado?"Sí":"No",530,3,65,40,event_action,(uintptr_t)((i<<2)|1U));
        button(row,"Editar",599,3,72,40,event_action,(uintptr_t)((i<<2)|2U));
        button(row,"X",675,3,48,40,event_action,(uintptr_t)((i<<2)|3U));
    }
}
static void history_render(void)
{
    lv_obj_clean(ui_contFilasHistorial); lv_obj_set_flex_flow(ui_contFilasHistorial,LV_FLEX_FLOW_COLUMN);
    for(unsigned i=0;i<app.history_n;i++) {
        record_t *r=&app.history[i]; lv_obj_t *row=lv_obj_create(ui_contFilasHistorial);
        lv_obj_set_size(row,740,54); lv_obj_set_style_pad_all(row,0,0); lv_obj_remove_flag(row,LV_OBJ_FLAG_SCROLLABLE);
        char date[28]; if(eq(r->date,"NA")) copy(date,sizeof(date),"Sin fecha");
        else snprintf(date,sizeof(date),"%.10s\n%.8s",r->date,r->date+11);
        label(row,date,3,2,140); char code[49]; copy(code,sizeof(code),r->code);
        for(char *p=code;*p;p++) if(*p=='_') *p=' ';
        label(row,code,148,13,245); label(row,r->detail,398,13,330);
    }
}

static bool record_parse(const line_t *l,record_t *r)
{
    if(!fields_allowed(l,"ID|REGISTRO_ID|FECHA|CODIGO|DESTINO|METODO|ORIGEN|MOTIVO|DURACION_S|VALOR|UNIDAD|EVENTO_ID|FIRMWARE|PH|EVENTOS|FASE|ALARMA|TIPO") ||
       !ascii_token(field(l,"REGISTRO_ID"),48)||!ascii_token(field(l,"CODIGO"),48)||!timestamp(field(l,"FECHA"))) return false;
    memset(r,0,sizeof(*r)); copy(r->id,sizeof(r->id),field(l,"REGISTRO_ID"));
    copy(r->date,sizeof(r->date),field(l,"FECHA")); copy(r->code,sizeof(r->code),field(l,"CODIGO"));
    const char *keys[]={"DESTINO","METODO","ORIGEN","MOTIVO","DURACION_S","VALOR","UNIDAD","EVENTO_ID"};
    for(unsigned i=0;i<8;i++) {
        const char *v=field(l,keys[i]); if(!v) continue;
        if(!ascii_token(v,48)) return false;
        if(i==0&&!one_of(v,"M1|M2|AMBOS|VERTICAL|PRINCIPAL")) return false;
        if(i==1&&!one_of(v,"NFT|INUNDACION|LLUVIA")) return false;
        if(i==2&&!one_of(v,"MANUAL|AUTO|AGENDA|HMI|PC")) return false;
        uint32_t n; if((i==4||i==7)&&!uint_value(v,&n)) return false;
        if(i==5) { char *end; double d=strtod(v,&end); if(end==v||*end||!isfinite(d)) return false; }
        size_t used=strlen(r->detail); snprintf(r->detail+used,sizeof(r->detail)-used,"%s%s%s",used?" / ":"",
            i==4?"s=":i==7?"evento=":"",v);
    }
    if(!r->detail[0]) copy(r->detail,sizeof(r->detail),"Sin detalle");
    return true;
}
/**
 * Acumula pagina en arrays temporales; solo incorpora filas tras FIN con
 * cantidad/snapshot validos. Deduplica IDs y respeta capacidad de vista.
 * Una pagina incompleta no se presenta como una agenda confirmada nueva.
 */
static bool transfer_line(const line_t *l,pending_t *p)
{
    kind_t kind=p->kind; transfer_t *x=kind==K_AGENDA?&app.agenda_x:kind==K_HISTORY?&app.history_x:&app.curves_x;
    const char *family=kind==K_AGENDA?"AGENDA":kind==K_HISTORY?"HISTORIAL":"CURVAS";
    if(x->id!=p->id) return false;
    if(l->words==2 && eq(l->word[0],family)&&eq(l->word[1],"INICIO")) {
        uint32_t total,rev=0; const char *snap=field(l,"SNAP");
        if(x->started||!number(l,"TOTAL",&total)||total>HMI_PAGINA||!ascii_token(snap,64)||
           !fields_allowed(l,kind==K_AGENDA?"ID|SNAP|TOTAL|REV_AGENDA":"ID|SNAP|TOTAL")) return false;
        if(kind==K_AGENDA&&!number(l,"REV_AGENDA",&rev)) return false;
        if(!x->first && !eq(x->snap,snap)) return false;
        if(kind==K_AGENDA&&!x->first&&rev!=x->rev) return false;
        copy(x->snap,sizeof(x->snap),snap); x->rev=rev; x->total=total; x->got=0; x->started=true;
        p->when=app.now; return true;
    }
    if(!x->started) return false;
    const char *row=kind==K_AGENDA?"EVENTO":kind==K_HISTORY?"REGISTRO":"MUESTRA";
    if(l->words==1 && eq(l->word[0],row)) {
        if(x->got>=x->total) return false;
        if(kind==K_AGENDA) {
            if(!event_parse(l,&app.agenda_page[x->got])) return false;
            for(unsigned i=0;i<x->got;i++) if(app.agenda_page[i].id==app.agenda_page[x->got].id) return false;
        } else if(kind==K_HISTORY) {
            if(!record_parse(l,&app.history_page[x->got])) return false;
            for(unsigned i=0;i<x->got;i++) if(eq(app.history_page[i].id,app.history_page[x->got].id)) return false;
        } else {
            if(!sample_parse(l,&app.curves_page[x->got],true)) return false;
            for(unsigned i=0;i<x->got;i++) if(eq(app.curves_page[i].boot,app.curves_page[x->got].boot) &&
                app.curves_page[i].num>=app.curves_page[x->got].num) return false;
        }
        x->got++; p->when=app.now; return true;
    }
    if(l->words!=2||!eq(l->word[0],family)||!eq(l->word[1],"FIN")) return false;
    uint32_t total; const char *next=field(l,"SIG");
    if(!fields_allowed(l,"ID|TOTAL|SIG")||!number(l,"TOTAL",&total)||total!=x->total||total!=x->got||
       !ascii_token(next,64)||(!total&&!eq(next,"FIN"))) return false;
    if(kind==K_AGENDA) {
        if(x->first) app.agenda_n=0;
        for(unsigned i=0;i<total;i++) {
            bool duplicate=false; for(unsigned j=0;j<app.agenda_n;j++) duplicate|=app.agenda[j].id==app.agenda_page[i].id;
            if(!duplicate && app.agenda_n<HMI_AGENDA_MAX) app.agenda[app.agenda_n++]=app.agenda_page[i];
        }
        agenda_render(); char text[100]; snprintf(text,sizeof(text),"%u eventos%s",app.agenda_n,
            app.agenda_n==HMI_AGENDA_MAX&&!eq(next,"FIN")?"; límite de vista":""); lv_label_set_text(app.agenda_status,text);
    } else if(kind==K_HISTORY) {
        if(x->first) app.history_n=0;
        for(unsigned i=0;i<total;i++) {
            bool duplicate=false; for(unsigned j=0;j<app.history_n;j++) duplicate|=eq(app.history[j].id,app.history_page[i].id);
            if(!duplicate && app.history_n<HMI_HISTORIAL_MAX) app.history[app.history_n++]=app.history_page[i];
        }
        history_render(); char text[100]; snprintf(text,sizeof(text),"%u registros%s",app.history_n,
            app.history_n==HMI_HISTORIAL_MAX&&!eq(next,"FIN")?"; límite de vista":""); lv_label_set_text(app.history_status,text);
    } else {
        if(!x->first && total && eq(app.curve_boot,app.curves_page[0].boot) &&
           app.curves_page[0].num<=app.curve_num) return false;
        if(x->first) curve_clear();
        for(unsigned i=0;i<total;i++) curve_add(&app.curves_page[i]);
        app.curve_pages++;
    }
    copy(x->cursor,sizeof(x->cursor),next); x->started=false; p->used=false;
    if(kind==K_CURVES) {
        if(!eq(next,"FIN") && app.curve_pages<(HMI_PUNTOS/HMI_PAGINA)) page_request(K_CURVES,false);
        else {
            if(app.have_data) curve_add(&app.data);
            lv_label_set_text(app.curve_status,eq(next,"FIN")?"Historial de mediciones recibido":"Ventana limitada; se conserva la muestra actual");
        }
    }
    if(kind==K_AGENDA && app.agenda_reload) { app.agenda_reload=false; page_request(K_AGENDA,true); }
    controls(); return true;
}
/**
 * Actualiza modo, niveles y actuaciones solo con un ESTADO valido.
 * Un BOOT diferente obliga a renegociar sesion; una revision vieja se ignora.
 */
static bool state_line(const line_t *l)
{
    if(l->words!=1||!eq(l->word[0],"ESTADO")||!fields_allowed(l,
       "ID|BOOT|REV|MODO|NIVEL_P|NIVEL_M1|NIVEL_M2|MOV_M1|MOV_M2|NFT_M1|NFT_M2|INUND_M1|INUND_M2|LLUVIA|REGLA|ALARMA")) return false;
    uint32_t rev; if(!ascii_token(field(l,"BOOT"),16)||!number(l,"REV",&rev)||
       !one_of(field(l,"MODO"),"AUTO|MANUAL")||!one_of(field(l,"NIVEL_P"),"SEGURO|BAJO|DESCONOCIDO")||
       !one_of(field(l,"NIVEL_M1"),"SUFICIENTE|INSUFICIENTE|DESCONOCIDO")||
       !one_of(field(l,"NIVEL_M2"),"SUFICIENTE|INSUFICIENTE|DESCONOCIDO")||
       !one_of(field(l,"REGLA"),"INCOMPLETA|LISTA|BLOQUEADA|EN_EJECUCION")||!ascii_token(field(l,"ALARMA"),48)) return false;
    const char *wkeys[]={"NFT_M1","NFT_M2","INUND_M1","INUND_M2","LLUVIA"};
    for(unsigned i=0;i<5;i++) if(!one_of(field(l,wkeys[i]),"INACTIVO|INICIANDO|EN_MARCHA|DETENIENDO|FALLO")) return false;
    for(unsigned i=0;i<2;i++) if(!one_of(field(l,i?"MOV_M2":"MOV_M1"),"DETENIDO|SUBIENDO|BAJANDO|INCLINANDO|NIVELANDO|FALLO")) return false;
    if(!eq(field(l,"BOOT"),app.boot)) { restart_link(true); return false; }
    if(app.state_valid&&rev<app.rev) return true;
    app.state_valid=true; app.rev=rev; copy(app.mode,sizeof(app.mode),field(l,"MODO"));
    copy(app.level,sizeof(app.level),field(l,"NIVEL_P")); copy(app.rule,sizeof(app.rule),field(l,"REGLA"));
    for(unsigned i=0;i<5;i++) copy(app.water[i],sizeof(app.water[i]),field(l,wkeys[i]));
    for(unsigned i=0;i<2;i++) copy(app.move[i],sizeof(app.move[i]),field(l,i?"MOV_M2":"MOV_M1"));
    char text[160]; snprintf(text,sizeof(text),"M%u: %s | Regla: %s | Alarma: %s",module()+1,app.move[module()],app.rule,field(l,"ALARMA"));
    message(text); controls(); return true;
}
/**
 * No repite puntos con mismo numero y comprueba coherencia de sus valores.
 * La edad conservada nunca retrocede por recibir otra copia de la misma muestra.
 */
static bool data_line(const line_t *l)
{
    sample_t sample; if(l->words!=1||!eq(l->word[0],"DATOS")||!sample_parse(l,&sample,false)) return false;
    if(!eq(sample.boot,app.boot)) { restart_link(true); return false; }
    bool same=app.have_data && eq(sample.boot,app.data.boot) && sample.num==app.data.num;
    if(app.have_data&&sample.num<app.data.num) return true;
    if(same) for(unsigned i=0;i<5;i++) {
        if(sample.quality[i]!=app.data.quality[i] || !eq(sample.ts[i],app.data.ts[i]) ||
           ((sample.quality[i]==Q_OK || sample.quality[i]==Q_PROVISIONAL) && sample.value[i]!=app.data.value[i])) return false;
        uint64_t age=(uint64_t)app.data.age[i]+(uint32_t)(app.now-app.data.received);
        if(!sample.age_na[i]&&age>sample.age[i]) sample.age[i]=age>UINT32_MAX?UINT32_MAX:(uint32_t)age;
    }
    app.data=sample; app.have_data=true; values();
    if(!same&&!busy_kind(K_CURVES)) curve_add(&sample);
    return true;
}
static bool threshold_line(const line_t *l,bool ack,pending_t *p)
{
    uint32_t value,rev,persistent;
    if(!fields_allowed(l,"ID|VALOR|UNIDAD|REV_CONFIG|PERSISTENTE")||!number(l,"VALOR",&value)||value>100||
       !eq(field(l,"UNIDAD"),"PCT")||!number(l,"REV_CONFIG",&rev)||!number(l,"PERSISTENTE",&persistent)||persistent>1) return false;
    if(ack && value!=(uint32_t)p->value) return false;
    if(ack) app.dirty=lv_spinbox_get_value(ui_spnUmbralHumedad)!=(int)value;
    set_threshold((int)value,rev,persistent!=0); return true;
}
static const char *error_text(const char *code)
{
    if(eq(code,"NIVEL_BAJO")) return "Rechazado: nivel de agua bajo";
    if(eq(code,"MODO_INCORRECTO")) return "Rechazado: modo incorrecto";
    if(eq(code,"LIMITE_ACTIVO")) return "El final de carrera ya está activo";
    if(eq(code,"SIN_REFERENCIA")) return "El módulo necesita referencia";
    if(eq(code,"CONFIG_INCOMPLETA")) return "Configuración automática incompleta";
    if(eq(code,"RTC_INVALIDO")) return "El reloj del central no es válido";
    if(eq(code,"CONFLICTO_AGENDA")) return "Conflicto con otro evento de agenda";
    if(eq(code,"OCUPADO")) return "Recurso ocupado";
    return code;
}
static bool clock_line(const line_t *l,bool ack,pending_t *p)
{
    uint32_t valid;
    if(!fields_allowed(l,"ID|VALIDO|FECHA|HORA|ZONA") || !number(l,"VALIDO",&valid) || valid>1) return false;
    if(ack && (!valid || !eq(field(l,"FECHA"),p->target) || !eq(field(l,"HORA"),p->method))) return false;
    return hmi_reloj_recibir(field(l,"FECHA"),field(l,"HORA"),field(l,"ZONA"),valid!=0,app.now);
}
/**
 * Despacha por el tipo de peticion pendiente y comprueba ID/familia/campos.
 * OK AGENDA confirma el guardado; ACEPTADO RIEGO/MOVER admite la solicitud
 * y exige consultar estado para saber como avanza o termina.
 */
static bool response(const line_t *l,pending_t *p)
{
    if(eq(l->word[0],"ERROR") && l->words==1) {
        const char *code=field(l,"CODIGO");
        if(!fields_allowed(l,"ID|CODIGO")||!ascii_token(code,48)) return false;
        message(error_text(code));
        if(p->kind==K_EVENT_SAVE) ui_evento_confirmar(p->token,false,0,error_text(code));
        if(p->kind==K_SET_THRESHOLD) lv_label_set_text(app.threshold_status,error_text(code));
        if(p->kind==K_CLOCK || p->kind==K_SET_CLOCK) hmi_reloj_error(error_text(code));
        if(p->kind==K_AGENDA) { app.agenda_x.cursor[0]=0; lv_label_set_text(app.agenda_status,error_text(code)); }
        if(p->kind==K_HISTORY) { app.history_x.cursor[0]=0; lv_label_set_text(app.history_status,error_text(code)); }
        if(p->kind==K_CURVES) { app.curves_x.cursor[0]=0; lv_label_set_text(app.curve_status,error_text(code));
            if(app.have_data) curve_add(&app.data); }
        p->used=false; controls(); return true;
    }
    if(p->kind==K_PING) { if(l->words!=1||!eq(l->word[0],"PONG")||!fields_allowed(l,"ID")) return false;
        p->used=false; return true; }
    if(p->kind==K_DATA) { if(!data_line(l)) return false; p->used=false; return true; }
    if(p->kind==K_STATE) { if(!state_line(l)) return false; p->used=false; return true; }
    if(p->kind==K_THRESHOLD) { if(l->words!=1||!eq(l->word[0],"UMBRAL")||!threshold_line(l,false,p)) return false;
        p->used=false; return true; }
    if(p->kind==K_CLOCK) { if(l->words!=1 || !eq(l->word[0],"RELOJ") || !clock_line(l,false,p)) return false;
        p->used=false; controls(); return true; }
    if(p->kind==K_AGENDA||p->kind==K_HISTORY||p->kind==K_CURVES) return transfer_line(l,p);
    if(l->words!=2||!one_of(l->word[0],"OK|ACEPTADO")) return false;
    bool accepted=eq(l->word[0],"ACEPTADO"); uint32_t rev,id,action,persistent;
    if(p->kind==K_ACK) {
        uint32_t ack;
        if(accepted || !eq(l->word[1],"ALARMAS") || !fields_allowed(l,"ID|RECONOCIDA") ||
           !number(l,"RECONOCIDA",&ack) || ack!=1) return false;
    } else if(p->kind==K_MODE) {
        if(accepted||!eq(l->word[1],"MODO")||!fields_allowed(l,"ID|VALOR|REV")||!number(l,"REV",&rev)||
           !eq(field(l,"VALOR"),p->value?"MANUAL":"AUTO")) return false;
        if(!app.state_valid||rev>=app.rev) { copy(app.mode,sizeof(app.mode),field(l,"VALOR")); app.rev=rev; }
    } else if(p->kind==K_SET_CLOCK) {
        if(accepted || !eq(l->word[1],"RELOJ") || !clock_line(l,true,p)) return false;
        hmi_reloj_confirmado();
    } else if(p->kind==K_SET_THRESHOLD) {
        if(accepted||!eq(l->word[1],"UMBRAL")||!threshold_line(l,true,p)) return false;
    } else if(p->kind==K_EVENT_SAVE||p->kind==K_EVENT_CHANGE) {
        if(accepted||!eq(l->word[1],"AGENDA")||!fields_allowed(l,"ID|EVENTO_ID|REV_AGENDA|PERSISTENTE")||
           !number(l,"EVENTO_ID",&id)||!id||!number(l,"REV_AGENDA",&rev)||
           !number(l,"PERSISTENTE",&persistent)||persistent>1 || (p->aux&&id!=p->aux)) return false;
        if(p->kind==K_EVENT_SAVE) ui_evento_confirmar(p->token,true,id,NULL);
        p->used=false;
        if(busy_kind(K_AGENDA)) app.agenda_reload=true; else page_request(K_AGENDA,true);
        controls(); return true;
    } else if(p->kind==K_WATER||p->kind==K_MOVE||p->kind==K_STOP) {
        const char *family=p->kind==K_WATER?"RIEGO":p->kind==K_MOVE?"MOVER":"PARAR";
        if(!eq(l->word[1],family)||!eq(field(l,"DESTINO"),p->target)||
           !fields_allowed(l,"ID|DESTINO|METODO|ESTADO|ACCION")) return false;
        if(p->kind==K_WATER&&!eq(field(l,"METODO"),p->method)) return false;
        if(accepted) { if(!number(l,"ACCION",&action)||!action) return false; }
        else if(!one_of(field(l,"ESTADO"),p->kind==K_WATER?"INACTIVO|INICIANDO|EN_MARCHA|DETENIENDO|FALLO":
                       "DETENIDO|SUBIENDO|BAJANDO|INCLINANDO|NIVELANDO|FALLO")) return false;
    } else return false;
    p->used=false; message(accepted?"Orden aceptada; consultando estado":"Cambio confirmado por el central");
    state_request(); controls(); return true;
}
/**
 * Reconcilia ORDEN VER de una modificacion incierta. Si se desconoce el
 * resultado no repite la orden. ESTADO=RESUELTA describe la peticion,
 * no prueba fin de una maniobra mecanica o de riego.
 */
static bool result_line(const line_t *l,pending_t *p)
{
    uint32_t old;
    if(eq(l->word[0],"ERROR") && eq(field(l,"CODIGO"),"RESULTADO_DESCONOCIDO") &&
       l->words==1 && fields_allowed(l,"ID|CODIGO")) {
        p->used=false; message("Resultado incierto: revisar estado y agenda; no se repite la orden");
        return true;
    }
    if(l->words!=1||!eq(l->word[0],"ORDEN")||!app.uncertain.used||!number(l,"NUM",&old)||
       old!=app.uncertain.id||old!=p->aux||!eq(field(l,"ESTADO"),"RESUELTA")||
       !one_of(field(l,"RESPUESTA"),"OK|ACEPTADO|ERROR")||!ascii_token(field(l,"FAMILIA"),16)) return false;
    if(!eq(field(l,"RESPUESTA"),"ERROR") && one_of(field(l,"FAMILIA"),"RIEGO|MOVER|PARAR")) {
        const char *expected=app.uncertain.kind==K_WATER?"RIEGO":app.uncertain.kind==K_MOVE?"MOVER":app.uncertain.kind==K_STOP?"PARAR":"";
        if(!eq(field(l,"FAMILIA"),expected)) return false;
        /* ORDEN usa ESTADO=RESUELTA. El estado de proceso se recupera de
         * ESTADO VER; no se inventa un campo de finalización mecánica. */
        app.uncertain.used=false; p->used=false; state_request(); controls();
        message("Resultado original conocido; consultando el estado actual"); return true;
    }
    char text[1025]; int n=snprintf(text,sizeof(text),"%s%s%s ID=%lu",field(l,"RESPUESTA"),
       eq(field(l,"RESPUESTA"),"ERROR")?"":" ",eq(field(l,"RESPUESTA"),"ERROR")?"":field(l,"FAMILIA"),
       (unsigned long)old);
    if(n<0 || (size_t)n>=sizeof(text)) return false;
    for(unsigned i=0;i<l->fields;i++) {
        if(one_of(l->key[i],"ID|NUM|ESTADO|RESPUESTA|FAMILIA")) continue;
        size_t used=strlen(text); int add=snprintf(text+used,sizeof(text)-used," %s=%s",l->key[i],l->val[i]);
        if(add<0||(size_t)add>=sizeof(text)-used) return false;
    }
    line_t original; if(!parse(&original,text)||!response(&original,&app.uncertain)) return false;
    p->used=false; controls(); return true;
}
static bool notice(const line_t *l)
{
    if(l->words!=2||!eq(l->word[0],"AVISO")) return false;
    uint32_t rev;
    if(eq(l->word[1],"AGENDA")) { if(!fields_allowed(l,"REV_AGENDA")||!number(l,"REV_AGENDA",&rev)) return false;
        if(busy_kind(K_AGENDA)) app.agenda_reload=true; else page_request(K_AGENDA,true); return true; }
    if(eq(l->word[1],"CONFIG")) { if(!fields_allowed(l,"REV_CONFIG")||!number(l,"REV_CONFIG",&rev)) return false;
        threshold_request(); return true; }
    /* AVISO actualiza mediante una consulta completa para evitar mezclar
     * campos parciales o tomar un aviso de aceptación como fin de maniobra. */
    if(!number(l,"REV",&rev)||!one_of(l->word[1],"MODO|NIVEL|MOVIMIENTO|RIEGO|ALARMA")) return false;
    if(!fields_allowed(l,"VALOR|REV|DESTINO|ESTADO|ACCION|ORIGEN|ORDEN|MOTIVO|METODO|EVENTO_ID|CODIGO|ACTIVA|FECHA")) return false;
    if(eq(l->word[1],"MODO")&&!one_of(field(l,"VALOR"),"AUTO|MANUAL")) return false;
    if(eq(l->word[1],"NIVEL") && (!one_of(field(l,"DESTINO"),"PRINCIPAL|M1|M2")||
        !one_of(field(l,"ESTADO"),eq(field(l,"DESTINO"),"PRINCIPAL")?"SEGURO|BAJO|DESCONOCIDO":"SUFICIENTE|INSUFICIENTE|DESCONOCIDO"))) return false;
    if(eq(l->word[1],"MOVIMIENTO")||eq(l->word[1],"RIEGO")) {
        uint32_t action;
        if(!number(l,"ACCION",&action)||!action||!one_of(field(l,"ORIGEN"),"HMI|PC|AUTO|AGENDA")||
           !one_of(field(l,"DESTINO"),eq(l->word[1],"MOVIMIENTO")?"M1|M2":"M1|M2|VERTICAL")) return false;
        if(!one_of(field(l,"ESTADO"),eq(l->word[1],"MOVIMIENTO")?
            "SUBIENDO|BAJANDO|INCLINANDO|NIVELANDO|FINALIZADO|CANCELADO|FALLIDO":
            "INICIANDO|EN_MARCHA|DETENIENDO|FINALIZADO|CANCELADO|FALLIDO")) return false;
        if(eq(l->word[1],"RIEGO")&&!one_of(field(l,"METODO"),"NFT|INUNDACION|LLUVIA")) return false;
    }
    if(eq(l->word[1],"ALARMA")) { uint32_t active;
        if(!number(l,"ACTIVA",&active)||active>1||!ascii_token(field(l,"CODIGO"),48)||!timestamp(field(l,"FECHA"))) return false; }
    if(!app.state_valid||rev>=app.rev) state_request();
    return true;
}
/**
 * Negocia HOLA, verifica respuestas contra pendientes y atiende avisos.
 * Solo mensajes validos actualizan el latido; bytes recibidos no bastan.
 * Un reinicio central invalida vistas sin reenviar actuaciones anteriores.
 */
static void receive_line(const char *text)
{
    line_t l; if(!parse(&l,text)) return;
    if(l.words==1&&eq(l.word[0],"HOLA")) {
        uint32_t version,next; const char *boot=field(&l,"BOOT");
        if(!app.hello_wait||!fields_allowed(&l,"VERSION|BOOT|PROX_ID")||!number(&l,"VERSION",&version)||version!=2||
           !number(&l,"PROX_ID",&next)||!next||!ascii_token(boot,16)) return;
        bool changed=app.boot[0]&&!eq(app.boot,boot);
        if(changed && app.uncertain.used) message("El central reinició; la orden anterior sigue incierta");
        if(changed) clear_session_views();
        copy(app.boot,sizeof(app.boot),boot); app.next_id=next; app.linked=true; app.hello_wait=false;
        app.last_rx=app.now; app.state_valid=false; app.cfg_valid=false;
        message("Enlace conectado; recuperando estado"); state_request(); threshold_request();
        clock_request();
        request("DATOS VER",K_DATA,0,0,0); app.data_at=app.state_at=app.now;
        if(lv_screen_active()==ui_ProgramacionPantalla) page_request(K_AGENDA,true);
        if(lv_screen_active()==ui_HistorialPantalla) page_request(K_HISTORY,true);
        if(lv_screen_active()==ui_VariablesPantalla) page_request(K_CURVES,true);
        if(app.uncertain.used && eq(app.boot,app.uncertain_boot)) { char command[64]; snprintf(command,sizeof(command),"ORDEN VER NUM=%lu",(unsigned long)app.uncertain.id);
            request(command,K_RESULT,app.uncertain.id,0,0); }
        controls(); return;
    }
    if(!app.linked) return;
    if(eq(l.word[0],"AVISO")) { if(notice(&l)) app.last_rx=app.now; return; }
    uint32_t id; if(!number(&l,"ID",&id)||!id) return;
    pending_t *p=find(id); if(!p && app.uncertain.used&&eq(app.boot,app.uncertain_boot)&&id==app.uncertain.id) p=&app.uncertain;
    if(!p) return;
    bool valid=p->kind==K_RESULT?result_line(&l,p):response(&l,p);
    if(valid) app.last_rx=app.now;
}

/**
 * Sincroniza slider/spinbox como borrador. Aplicar envia la solicitud;
 * que dos widgets coincidan no implica que el central haya aceptado el umbral.
 */
static void threshold_changed(lv_event_t *e)
{
    if(app.syncing) return;
    lv_obj_t *source=lv_event_get_current_target(e);
    int value=source==ui_sldUmbralHumedad?(int)lv_slider_get_value(source):(int)lv_spinbox_get_value(source);
    app.syncing=true;
    if(lv_slider_get_value(ui_sldUmbralHumedad)!=value) lv_slider_set_value(ui_sldUmbralHumedad,value,LV_ANIM_OFF);
    if(lv_spinbox_get_value(ui_spnUmbralHumedad)!=value) lv_spinbox_set_value(ui_spnUmbralHumedad,value);
    app.syncing=false; app.dirty=!app.cfg_valid||value!=app.confirmed;
    lv_label_set_text(app.threshold_status,app.dirty?"Cambios sin aplicar":"Umbral confirmado"); controls();
}
static void unit_step(lv_event_t *e)
{ (void)e; lv_spinbox_set_step(ui_spnUmbralHumedad,1); }
/**
 * Interpreta el switch como deseo de modo y restaura la vista confirmada
 * mientras espera respuesta; la interfaz no declara AUTO por un clic local.
 */
static void mode_clicked(lv_event_t *e)
{
    (void)e; bool desired=lv_obj_has_state(ui_swModoOperacion,LV_STATE_CHECKED);
    bool allowed=app.linked&&app.state_valid&&!busy_mutation(); controls();
    if(!allowed) { message("Esperá el estado confirmado del central"); return; }
    if(!request(desired?"MODO MANUAL":"MODO AUTO",K_MODE,0,0,desired)) message("No se pudo enviar el modo");
    else message("Solicitando cambio de modo...");
}
static void module_changed(lv_event_t *e) { (void)e; controls(); state_request(); }
static void move_clicked(lv_event_t *e)
{
    if(!app.state_valid||!eq(app.mode,"MANUAL")||busy_mutation()) return;
    unsigned action=(unsigned)(uintptr_t)lv_event_get_user_data(e); if(action>=4) return;
    const char *names[]={"SUBIR","BAJAR","INCLINAR","NIVELAR"}; unsigned m=module(); char command[64];
    snprintf(command,sizeof(command),"MOVER M%u %s",m+1,names[action]);
    pending_t *p=request(command,K_MOVE,0,0,0);
    if(p) { snprintf(p->target,sizeof(p->target),"M%u",m+1); message("Movimiento solicitado; esperando admisión"); }
    else message("No se pudo enviar el movimiento");
}
static void stop_clicked(lv_event_t *e)
{
    (void)e; unsigned m=module(); char command[32]; snprintf(command,sizeof(command),"PARAR M%u",m+1);
    pending_t *p=request(command,K_STOP,0,0,0);
    if(p) { snprintf(p->target,sizeof(p->target),"M%u",m+1); message("Parar solicitado; esperando confirmación"); }
    else message("Sin enlace para enviar Parar");
}
/**
 * ON/OFF depende del ultimo estado confirmado del metodo/destino.
 * No controla GPIO; prepara RIEGO y deja ejecucion/protecciones al central.
 */
static void water_clicked(lv_event_t *e)
{
    if(!app.state_valid) return;
    unsigned kind=(unsigned)(uintptr_t)lv_event_get_user_data(e);
    if(kind>2) return;
    unsigned m=module(),index=kind==2?4:kind==1?m+2:m; bool on=!active_water(index);
    if(on && (!eq(app.mode,"MANUAL")||busy_mutation())) return;
    const char *method=kind==2?"LLUVIA":kind==1?"INUNDACION":"NFT";
    char target[12],command[72]; if(kind==2) copy(target,sizeof(target),"VERTICAL"); else snprintf(target,sizeof(target),"M%u",m+1);
    snprintf(command,sizeof(command),"RIEGO %s %s %s",target,method,on?"ON":"OFF");
    pending_t *p=request(command,K_WATER,0,0,on);
    if(p) { copy(p->target,sizeof(p->target),target); copy(p->method,sizeof(p->method),method);
        message("Riego solicitado; esperando admisión"); } else message("No se pudo enviar el riego");
}
void hmi_app_aplicar_umbral(lv_event_t *e)
{
    (void)e; if(!app.linked||!app.cfg_valid||!app.dirty) return;
    int value=(int)lv_spinbox_get_value(ui_spnUmbralHumedad); if(value<0||value>100) return;
    char command[32]; snprintf(command,sizeof(command),"UMBRAL %d",value);
    if(request(command,K_SET_THRESHOLD,0,0,value)) lv_label_set_text(app.threshold_status,"Aplicando; esperando confirmación");
    else lv_label_set_text(app.threshold_status,"No se pudo enviar el umbral");
}
/**
 * Convierte borrador a AGENDA AGREGAR/EDITAR con duracion en segundos y dias
 * ordenados. El token identifica el formulario y el ID UART lo asigna request.
 */
static bool save_event(const hmi_evento_t *event,uint32_t token,void *context)
{
    (void)context; if(hmi_evento_validar(event)) return false;
    char command[300],prefix[64],days[32]; days_text(event->dias,days,sizeof(days));
    if(event->id) snprintf(prefix,sizeof(prefix),"AGENDA EDITAR EVENTO_ID=%lu",(unsigned long)event->id);
    else copy(prefix,sizeof(prefix),"AGENDA AGREGAR");
    snprintf(command,sizeof(command),"%s HORA=%02u:%02u METODO=%s DESTINO=%s DURACION_S=%lu DIAS=%s HABILITADO=%u",
        prefix,event->hora,event->minuto,method_name(event->metodo),target_name(event->destino),
        (unsigned long)event->duracion_s,days,event->habilitado?1U:0U);
    return request(command,K_EVENT_SAVE,event->id,token,0)!=NULL;
}
static void event_saved(const hmi_evento_t *event,void *context)
{
    /* El receptor del OK programa una única recarga de agenda. */
    (void)event; (void)context;
}
void hmi_app_abrir_evento(lv_event_t *e)
{ if(app.linked&&!busy_mutation()) ui_evento_agregar_clicked(e); else message("Sin conexión o hay una orden pendiente"); }
void hmi_app_cargar_historial(lv_event_t *e) { (void)e; page_request(K_HISTORY,true); }
void hmi_app_actualizar_historial(lv_event_t *e) { (void)e; page_request(K_HISTORY,true); }
static void more_clicked(lv_event_t *e)
{ page_request((kind_t)(uintptr_t)lv_event_get_user_data(e),false); }
static void agenda_refresh(lv_event_t *e) { (void)e; page_request(K_AGENDA,true); }
static void screen_loaded(lv_event_t *e)
{
    lv_obj_t *screen=lv_event_get_current_target(e);
    if(screen==ui_ProgramacionPantalla) { page_request(K_AGENDA,true); threshold_request(); }
    if(screen==ui_VariablesPantalla) page_request(K_CURVES,true);
    if(screen==ui_ModoManualPantalla) state_request();
    values(); controls();
}
static void close_popup(lv_event_t *e)
{ (void)e; if(app.popup) { lv_obj_delete(app.popup); app.popup=NULL; } }
static void delete_confirmed(lv_event_t *e)
{
    (void)e; char command[80]; snprintf(command,sizeof(command),"AGENDA ELIMINAR EVENTO_ID=%lu",(unsigned long)app.delete_id);
    if(request(command,K_EVENT_CHANGE,app.delete_id,0,0)) message("Eliminación solicitada; esperando confirmación");
    else message("No se pudo solicitar la eliminación");
    close_popup(NULL);
}
static lv_obj_t *popup(const char *text)
{
    if(app.popup) return NULL;
    app.popup=lv_obj_create(lv_screen_active()); lv_obj_set_pos(app.popup,0,0); lv_obj_set_size(app.popup,800,480);
    lv_obj_remove_flag(app.popup,LV_OBJ_FLAG_SCROLLABLE); lv_obj_set_style_bg_color(app.popup,lv_color_hex(0xEFF4F6),0);
    lv_obj_set_style_bg_opa(app.popup,LV_OPA_COVER,0);
    lv_obj_t *t=label(app.popup,text,30,35,700); lv_label_set_long_mode(t,LV_LABEL_LONG_WRAP);
    return app.popup;
}
/**
 * Decodifica fila/accion de los botones: habilitar, editar o solicitar
 * eliminacion con confirmacion. Solo usa IDs de agenda recibidos del central.
 */
static void event_action(lv_event_t *e)
{
    uintptr_t data=(uintptr_t)lv_event_get_user_data(e); unsigned index=(unsigned)(data>>2),action=(unsigned)(data&3);
    if(index>=app.agenda_n || !app.linked || busy_mutation()) return;
    hmi_evento_t *event=&app.agenda[index];
    if(action==2) { if(!ui_evento_editar(lv_screen_active(),event)) message("Duración fuera de capacidad del editor o formulario abierto"); return; }
    if(action==3) {
        app.delete_id=event->id; if(popup("¿Eliminar este evento de la agenda?\nSe enviará una solicitud al central.")) {
            button(app.popup,"Cancelar",100,340,230,60,close_popup,0);
            button(app.popup,"Eliminar",430,340,230,60,delete_confirmed,0); }
        return;
    }
    char command[90]; snprintf(command,sizeof(command),"AGENDA %s EVENTO_ID=%lu",event->habilitado?"DESACTIVAR":"ACTIVAR",(unsigned long)event->id);
    if(request(command,K_EVENT_CHANGE,event->id,0,0)) message("Cambio de habilitación solicitado");
}
static void acknowledge_uncertain(lv_event_t *e)
{
    (void)e; ui_evento_cerrar_incierta(app.uncertain.token); app.uncertain.used=false;
    message("Resultado reconocido por el usuario; no se retransmitió la orden");
    close_popup(NULL); controls();
}
static void reconocer_alarma(lv_event_t *e)
{
    (void)e;
    if(app.linked && request("ALARMAS RECONOCER",K_ACK,0,0,0)) {
        message("Reconocimiento solicitado al central"); close_popup(NULL);
    }
}
static void diagnostic(lv_event_t *e)
{
    (void)e; char text[720]; snprintf(text,sizeof(text),
        "Enlace: %s\nCentral: %s\nModo: %s\nRegla: %s\nUmbral confirmado: %s\nMuestra: %lu\n%s",
        app.linked?"Conectado":"Sin conexión",app.boot[0]?app.boot:"Sin datos",app.state_valid?app.mode:"Sin datos",
        app.state_valid?app.rule:"Sin datos",app.cfg_valid?"Disponible":"Sin datos",(unsigned long)(app.have_data?app.data.num:0),
        app.uncertain.used?"Hay una orden sin resultado conocido. Revisá el estado antes de reconocerla.":"El central ejecuta las protecciones. pH con ~: ajuste provisional.");
    if(!popup(text)) return;
    button(app.popup,"Cerrar",60,340,180,60,close_popup,0);
    if(app.uncertain.used) button(app.popup,"Revisé el resultado",300,340,340,60,acknowledge_uncertain,0);
    else button(app.popup,"Reconocer alarma",300,340,340,60,reconocer_alarma,0);
    button(app.popup,"Fecha y hora",60,270,240,50,hmi_reloj_abrir,0);
    state_request(); page_request(K_AGENDA,true);
}
/**
 * Conserva la modificacion sin resultado para consultar/reconocer, sin
 * retransmitirla. Las paradas prioritarias se reconcilian mediante ESTADO.
 */
static void remember_uncertain(const pending_t *p)
{
    /* Las cancelaciones prioritarias se reconcilian por ESTADO. Se reserva
     * este registro para la modificación normal que bloquea nuevas altas. */
    if(p->kind==K_STOP || (p->kind==K_WATER&&!p->value)) { state_request(); return; }
    if(!app.uncertain.used) { app.uncertain=*p; app.uncertain.used=true;
        copy(app.uncertain_boot,sizeof(app.uncertain_boot),app.boot); }
    if(p->kind==K_EVENT_SAVE) ui_evento_incierta(p->token,"Sin respuesta: el resultado es incierto. Cerrar no vuelve a enviar.");
    if(p->kind==K_SET_CLOCK) hmi_reloj_error("Sin confirmación. Se consulta el resultado; no se repite el ajuste.");
}
static void clear_session_views(void)
{
    app.have_data=false; curve_clear();
    app.agenda_n=app.history_n=0; app.agenda_reload=false;
    memset(&app.agenda_x,0,sizeof(app.agenda_x));
    memset(&app.history_x,0,sizeof(app.history_x));
    memset(&app.curves_x,0,sizeof(app.curves_x));
    agenda_render(); history_render();
}
/**
 * Invalida enlace y convierte cambios pendientes en inciertos. Reintenta
 * HOLA, no ON/Guardar. Si cambio BOOT, retira vistas de la sesion anterior.
 */
static void restart_link(bool central_restarted)
{
    /* PONG puede seguir llegando después de un reinicio corto. Un BOOT nuevo
     * en una respuesta válida exige otro HOLA y nuevos identificadores.
     * Las órdenes pendientes se conservan como inciertas, nunca se repiten. */
    app.linked=false; app.hello_wait=false; app.hello_at=app.now-HMI_HOLA_MS;
    for(unsigned i=0;i<12;i++) if(app.pending[i].used) {
        if(mutating(app.pending[i].kind)) remember_uncertain(&app.pending[i]);
        app.pending[i].used=false;
    }
    app.state_valid=false; app.cfg_valid=false;
    hmi_reloj_estado(false,false,busy_mutation());
    if(central_restarted) {
        clear_session_views();
        message("El central reinició; sincronizando de nuevo...");
    } else message("Sin conexión; el central debe supervisar el control manual");
    lv_label_set_text(app.threshold_status,"Sin conexión");
    lv_label_set_text(app.history_status,central_restarted?"Historial por consultar":"Sin conexión; vista anterior");
    lv_label_set_text(app.agenda_status,central_restarted?"Agenda por consultar":"Sin conexión; vista anterior");
    if(central_restarted) lv_label_set_text(app.curve_status,"Mediciones por consultar");
    controls(); values();
}
/**
 * Inicializa despues de ui_init y antes del primer render. Sustituye los
 * arrays externos de un punto exportados por SquareLine por buffers persistentes
 * de HMI_PUNTOS, conecta callbacks y crea los elementos propios de la aplicacion.
 */
void hmi_app_init(hmi_transmitir_t transmit,void *context,uint32_t now)
{
    memset(&app,0,sizeof(app)); app.tx=transmit; app.context=context; app.now=now; app.hello_at=now-HMI_HOLA_MS;
    hmi_reloj_init(ui_PrincipalPantalla,clock_request,clock_save);
    app.status=label(ui_PrincipalPantalla,"Conectando con el central...",12,397,770);
    app.manual_status=label(ui_ModoManualPantalla,"Sin conexión",205,271,570);
    app.threshold_status=label(ui_ProgramacionPantalla,"Sin configuración del central",160,449,620);
    app.agenda_status=label(ui_ProgramacionPantalla,"Agenda sin consultar",333,332,445);
    app.history_status=label(ui_HistorialPantalla,"Historial sin consultar",16,340,760);
    app.curve_status=label(ui_VariablesPantalla,"Sin mediciones",170,446,600);
    button(ui_ProgramacionPantalla,"Actualizar",333,279,165,44,agenda_refresh,0);
    app.agenda_more=button(ui_ProgramacionPantalla,"Más eventos",507,373,180,42,more_clicked,K_AGENDA);
    app.history_more=button(ui_HistorialPantalla,"Más",566,280,108,49,more_clicked,K_HISTORY);
    const int32_t min[]={0,0,150,0},max[]={1400,2000,350,1000};
    for(unsigned i=0;i<4;i++) {
        lv_obj_t *chart=charts(i); app.series[i]=lv_chart_get_series_next(chart,NULL);
        /* Remover el puntero externo insuficiente antes de cambiar capacidad.
         * Esta UI crea un único arreglo externo: se reemplaza antes de dibujar. */
        lv_chart_set_series_ext_y_array(chart,app.series[i],app.points[i]);
        lv_chart_set_point_count(chart,HMI_PUNTOS); lv_chart_set_axis_range(chart,LV_CHART_AXIS_PRIMARY_Y,min[i],max[i]);
        lv_chart_set_update_mode(chart,LV_CHART_UPDATE_MODE_SHIFT);
        lv_obj_set_scrollbar_mode(chart,LV_SCROLLBAR_MODE_OFF); lv_obj_remove_flag(chart,LV_OBJ_FLAG_SCROLLABLE);
    }
    curve_clear();
    lv_slider_set_range(ui_sldUmbralHumedad,0,100); lv_spinbox_set_range(ui_spnUmbralHumedad,0,100);
    lv_spinbox_set_rollover(ui_spnUmbralHumedad,false); lv_spinbox_set_step(ui_spnUmbralHumedad,1);
    lv_obj_add_event_cb(ui_spnUmbralHumedad,threshold_changed,LV_EVENT_VALUE_CHANGED,NULL);
    lv_obj_add_event_cb(ui_sldUmbralHumedad,threshold_changed,LV_EVENT_VALUE_CHANGED,NULL);
    lv_obj_add_event_cb(ui_btnAumentarUmbral,unit_step,LV_EVENT_PRESSED,NULL);
    lv_obj_add_event_cb(ui_btnDisminuirUmbral,unit_step,LV_EVENT_PRESSED,NULL);
    lv_obj_add_event_cb(ui_swModoOperacion,mode_clicked,LV_EVENT_VALUE_CHANGED,NULL);
    lv_obj_add_event_cb(ui_swModuloHorizontal,module_changed,LV_EVENT_VALUE_CHANGED,NULL);
    lv_obj_t *moves[]={ui_btnSubir,ui_btnBajar,ui_btnInclinar,ui_btnNivelar};
    for(unsigned i=0;i<4;i++) lv_obj_add_event_cb(moves[i],move_clicked,LV_EVENT_CLICKED,(void*)(uintptr_t)i);
    lv_obj_add_event_cb(ui_btnDetenerMovimiento,stop_clicked,LV_EVENT_CLICKED,NULL);
    lv_obj_t *water[]={ui_btnNFT,ui_btnInundacionDrenaje,ui_btnLluvia};
    for(unsigned i=0;i<3;i++) lv_obj_add_event_cb(water[i],water_clicked,LV_EVENT_CLICKED,(void*)(uintptr_t)i);
    lv_obj_add_event_cb(ui_btnConfiguracion,diagnostic,LV_EVENT_CLICKED,NULL);
    lv_obj_add_event_cb(ui_ProgramacionPantalla,screen_loaded,LV_EVENT_SCREEN_LOADED,NULL);
    lv_obj_add_event_cb(ui_VariablesPantalla,screen_loaded,LV_EVENT_SCREEN_LOADED,NULL);
    lv_obj_add_event_cb(ui_ModoManualPantalla,screen_loaded,LV_EVENT_SCREEN_LOADED,NULL);
    ui_evento_configurar(save_event,event_saved,NULL);
    controls(); values();
}
/**
 * Recibe un byte desde loop: acumula hasta LF/CRLF y descarta caracteres
 * invalidos, exceso de longitud o fragmentos. No usar desde una ISR de UART.
 */
void hmi_app_rx(uint8_t byte,uint32_t now)
{
    app.now=now; app.rx_at=now;
    if(byte=='\n') {
        if(!app.discard && app.input_len) { app.input[app.input_len]=0; receive_line(app.input); }
        app.input_len=0; app.discard=false; app.rx_cr=false; return;
    }
    if(app.discard) return;
    if(byte=='\r'&&!app.rx_cr) { app.rx_cr=true; return; }
    if(app.rx_cr || byte<32 || byte>126 || app.input_len>=HMI_LINEA_MAX) { app.discard=true; app.input_len=0; return; }
    app.input[app.input_len++]=(char)byte;
}
/**
 * Atiende reloj, linea incompleta, watchdog, vencimientos y consultas
 * periodicas. Para cambios sin confirmacion consulta resultado; no los reenvia.
 * Comparaciones por resta uint32_t soportan el desbordamiento de millis.
 */
void hmi_app_tick(uint32_t now)
{
    app.now=now;
    hmi_reloj_tick(now);
    if((app.input_len||app.rx_cr) && (uint32_t)(now-app.rx_at)>HMI_LINEA_MS) { app.input_len=0; app.discard=true; app.rx_cr=false; }
    if(app.linked && (uint32_t)(now-app.last_rx)>HMI_ENLACE_MS) restart_link(false);
    if(!app.linked) {
        if((uint32_t)(now-app.hello_at)>=HMI_HOLA_MS) { const char *hello="HOLA VERSION=2\n";
            if(app.tx(hello,strlen(hello),app.context)) { app.hello_at=now; app.hello_wait=true; } }
        return;
    }
    for(unsigned i=0;i<12;i++) { pending_t *p=&app.pending[i];
        if(!p->used || (uint32_t)(now-p->when)<HMI_RESPUESTA_MS) continue;
        if(mutating(p->kind)) {
            remember_uncertain(p); message("Sin confirmación: se consulta el resultado, sin repetir la orden");
            p->used=false;
            if(app.uncertain.used && eq(app.uncertain_boot,app.boot) && !busy_kind(K_RESULT)) {
                char command[64]; snprintf(command,sizeof(command),"ORDEN VER NUM=%lu",(unsigned long)app.uncertain.id);
                request(command,K_RESULT,app.uncertain.id,0,0); }
            state_request();
        } else {
            if(p->kind==K_AGENDA) {
                app.agenda_x.cursor[0]=0;
                lv_label_set_text(app.agenda_status,"Consulta incompleta; vista anterior");
            }
            if(p->kind==K_HISTORY) {
                app.history_x.cursor[0]=0;
                lv_label_set_text(app.history_status,"Consulta incompleta; vista anterior");
            }
            if(p->kind==K_CURVES) { lv_label_set_text(app.curve_status,"Historial incompleto; se muestran las muestras recibidas");
                if(app.have_data) curve_add(&app.data); }
            if(p->kind==K_CLOCK) hmi_reloj_error("Consulta de hora sin respuesta. Podés volver a leer del central.");
            p->used=false;
        }
    }
    if((uint32_t)(now-app.ping_at)>=HMI_PING_MS && !busy_kind(K_PING)) {
        if(request("PING",K_PING,0,0,0)) app.ping_at=now;
    }
    if((uint32_t)(now-app.data_at)>=HMI_CONSULTA_MS && !busy_kind(K_DATA)) {
        if(request("DATOS VER",K_DATA,0,0,0)) app.data_at=now;
    }
    if((uint32_t)(now-app.state_at)>=HMI_CONSULTA_MS && !busy_kind(K_STATE)) {
        if(request("ESTADO VER",K_STATE,0,0,0)) app.state_at=now;
    }
    if((uint32_t)(now-app.clock_at)>=HMI_RELOJ_CONSULTA_MS) clock_request();
    if((uint32_t)(now-app.labels_at)>=1000) { app.labels_at=now; values(); controls(); }
}
bool hmi_app_conectada(void) { return app.linked; }
uint32_t hmi_app_muestra(void) { return app.have_data?app.data.num:0; }
unsigned hmi_app_puntos_recibidos(void) { return app.curve_n; }
unsigned hmi_app_eventos(void) { return app.agenda_n; }
unsigned hmi_app_registros(void) { return app.history_n; }

/* Compiles the FIRMWARE'S OWN pv_json.c natively and prints the state
 * document it would emit, so it can be diffed against the live vent. */
#include "pv.h"
#include <stdio.h>
#include <string.h>
pv_cfg_t  g_cfg;
pv_live_t g_live;
extern char *pv_json_state(void);

static void fx_set(pv_fx_param_t *p, const char *c){ p->brightness=50; p->speed=50; snprintf(p->color,7,"%s",c); }
int main(void){
    /* factory defaults, mirroring pv_cfg.c */
    memset(&g_cfg,0,sizeof g_cfg);
    g_cfg.rgb.light_on=1; g_cfg.rgb.warning_sw=1; g_cfg.rgb.follow_vent=1;
    g_cfg.rgb.light_mode=0; g_cfg.rgb.simple_current=0;
    for(int i=0;i<PV_FX_COUNT;++i) fx_set(&g_cfg.rgb.simple[i],"FF3700");
    const char *sc[6]={"FFFFFF","FF8000","FFFFFF","FFFFFF","00FF00","FF0000"};
    const unsigned char sf[6]={0,0,6,0,0,0};
    for(int s=0;s<PV_ST_COUNT;++s){ g_cfg.rgb.h2d_active[s]=sf[s];
        for(int f=0;f<PV_FX_COUNT;++f) fx_set(&g_cfg.rgb.h2d[s][f],sc[s]); }
    for(int l=0;l<2;++l){ g_cfg.rgb.warnhot_current[l]=0;
        for(int f=0;f<2;++f){ g_cfg.rgb.warnhot_bg[l][f]=50; g_cfg.rgb.warnhot_speed[l][f]=50; } }
    snprintf(g_cfg.ap.ssid,33,"Panda_Vent_AABBCCDDEEFF");
    snprintf(g_cfg.ap.password,65,"987654321");
    snprintf(g_cfg.ap.ip,16,"192.168.254.1"); g_cfg.ap.on=1;
    snprintf(g_cfg.hostname,32,"PandaVent"); snprintf(g_cfg.language,6,"en");
    snprintf(g_cfg.printer.name,32,"Printer"); snprintf(g_cfg.printer.sn,24,"PLACEHOLDERSN0001");
    snprintf(g_cfg.printer.access_code,16,"redacted"); snprintf(g_cfg.printer.ip,16,"192.168.0.3");
    snprintf(g_live.sta_ssid,33,"your-ssid"); snprintf(g_live.sta_password,65,"redacted");
    snprintf(g_live.sta_ip,16,"192.168.0.2"); g_live.sta_state=3; g_live.printer_state=3;
    char *s = pv_json_state(); printf("%s\n", s); return 0;
}

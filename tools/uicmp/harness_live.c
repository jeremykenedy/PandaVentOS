/* Compiles the FIRMWARE'S OWN pv_json.c natively, seeded with the exact
 * values captured from the live stock device, and prints the state document
 * it would emit. If the clone is faithful this equals the live capture. */
#include "pv.h"
#include <stdio.h>
#include <string.h>
pv_cfg_t  g_cfg;
pv_live_t g_live;
extern char *pv_json_state(void);
int main(void){
    memset(&g_cfg,0,sizeof g_cfg);
    memset(&g_live,0,sizeof g_live);
#include "seed_live.h"
    char *s = pv_json_state(); printf("%s\n", s); return 0;
}

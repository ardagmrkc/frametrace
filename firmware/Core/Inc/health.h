#ifndef HEALTH_H
#define HEALTH_H

/*
 * Saglik izleyici (ARINC 653 Health Monitor fikrinin kucuk bir karsiligi).
 * Port katmani ve uygulama hatalari buraya bildirir; her olay turunun
 * onceden tanimli bir tepkisi vardir (health.c k_action):
 *   HM_LOG   say ve devam et
 *   HM_HALT  tasarim hatasi: configASSERT ile dur
 *
 * Birden cok gorev bildirebilir: sayac kritik bolumde artar (kesmeden
 * cagrilmaz). hm_reset yalnizca kosular arasinda cagrilir.
 */
#include <stdint.h>

typedef enum
{
    HM_PORT_OVERFLOW = 0, /* queuing port dolu, gonderim basarisiz (yazan) */
    HM_PORT_ALLOC_ERROR,  /* bos tutamac kuyrugundan gecersiz tutamac (yazan) */
    HM_PORT_FREE_ERROR,   /* gecersiz ya da beklenmeyen durumda tutamac alindi/geri verildi (okuyan) */
    HM_PORT_ACCESS,       /* porta yetkisiz gorev erisimi: dur */
    HM_STALE_READ,        /* sampling port: bayat okuma (okuyan) */
    HM_DEADLINE_MISS,     /* servis deadline'dan gec basladi (Arda cerceve gorevi) */
    HM_EVENT_COUNT
} hm_event_t;

void hm_report(hm_event_t e);
uint32_t hm_count(hm_event_t e);
void hm_reset(void);

#endif /* HEALTH_H */

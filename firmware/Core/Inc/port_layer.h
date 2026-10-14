#ifndef PORT_LAYER_H
#define PORT_LAYER_H

/*
 * Port katmani: ARINC 653'teki port fikrinin kucuk bir karsiligi. Gorevler
 * veri alisverisini yalnizca portlarla yapar; kuyruk, slot tablosu ve
 * sayaclar bu katmanin icindedir. Portlar port_config.c'deki tabloda
 * tanimlidir.
 *
 * Iki tur:
 *  - Queuing port (olay verisi): sira korunur, hicbiri atilmaz, kapasite
 *    tablodadir. Mesajlar statik slotlarda durur; kuyruklarda yalnizca 1
 *    baytlik slot tutamaci tasinir. Okuyan mesaji slotunda okur (kopyasiz)
 *    ve isi bitince slotu tam bir kez geri verir.
 *  - Sampling port (durum verisi): tek slot; yazan her seferinde ustune
 *    yazar. Okuyan son degeri, yasini ve gecerliligini (yas <= tazeleme
 *    suresi) alir; arada atlanan guncellemeler sayilir. Atlanan guncelleme
 *    sampling'in dogasidir, hata degildir; bayat okuma ise bildirilir.
 *
 * Kurallar:
 *  P1 Butun bellek statiktir; slot ve tutamac sayisi tablodan gelir.
 *  P2 Hicbir cagri beklemez (0 tick). Dolu/bos durumu donus degeriyle
 *     bildirilir.
 *  P3 Her portun tek yazari ve tek okuyucusu vardir (port_bind). Baska bir
 *     gorevin cagirmasi tasarim hatasidir (HM_PORT_ACCESS: dur).
 *  P4 Queuing port'ta her tutamac her an tek bir yerdedir; durumu
 *     BOS -> KUYRUKTA -> ALINDI -> BOS. Gecersiz tutamac slota erisim icin
 *     kullanilmaz, bildirilir.
 *  P5 Her hata saglik izleyiciye (health.h) bildirilir; tepkiyi o belirler.
 *  P6 Her sayacin tek yazari vardir: gonderme/yazma sayaclarini yazan,
 *     alma/okuma sayaclarini okuyan gorev yazar.
 *
 * port_reset ve port_bind yonetim cagrilaridir: yalnizca portu kimse
 * kullanmiyorken (scheduler oncesi ya da kosular arasinda) cagrilir.
 */
#include <stdbool.h>
#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "port_config.h"

typedef uint8_t port_handle_t;
#define PORT_HANDLE_NONE 0xFFU /* gecersiz tutamac; kapasite bundan kucuk olmali */

typedef enum
{
    PORT_QUEUING = 0,
    PORT_SAMPLING,
} port_kind_t;

typedef enum
{
    PORT_OK = 0,
    PORT_FULL,  /* queuing: bos slot yok (gonderim basarisiz) */
    PORT_EMPTY, /* queuing: bekleyen mesaj yok; sampling: hic yazilmadi */
    PORT_STALE, /* sampling: deger tazeleme suresinden eski */
    PORT_ERROR, /* gecersiz tutamac ya da durum; bildirildi */
} port_status_t;

/* Sampling port okumasinin ek bilgisi. */
typedef struct
{
    uint32_t age_us; /* degerin yazildigi andan okumaya */
    uint32_t missed; /* bir onceki okumadan bu yana okunmadan ustune yazilan deger */
    bool valid;      /* age_us <= tazeleme suresi */
} port_sample_info_t;

/* Port sayaclari (P6: her alanin tek yazari). Tutamac hatalari saglik
 * izleyicide sayilir (health.h). */
typedef struct
{
    /* queuing */
    uint32_t sent;           /* yazan */
    uint32_t overflow;       /* yazan: bos slot yoktu (gonderim basarisiz) */
    uint32_t received;       /* okuyan */
    /* sampling */
    uint32_t writes;         /* yazan; okuyan kritik bolumde okur */
    uint32_t reads;          /* okuyan */
    uint32_t stale_reads;    /* okuyan */
    uint32_t missed_updates; /* okuyan: okunmadan ustune yazilan deger, toplam */
    uint32_t age_max_us;     /* okuyan: okunan degerin en buyuk yasi */
} port_stats_t;

/* Port tanimi (port_config.c). Depolar 4 bayt hizali; mesaj boyu 4'un kati. */
typedef struct
{
    const char *name;
    port_kind_t kind;
    uint16_t msg_size;        /* bayt */
    uint8_t capacity;         /* queuing: slot sayisi (< PORT_HANDLE_NONE); sampling: 1 */
    uint32_t refresh_us;      /* sampling: bu yastan eski deger gecersiz; queuing: 0 */
    uint32_t *slots;          /* capacity x msg_size bayt */
    uint8_t *state;           /* queuing: slot durumu, capacity eleman */
    uint8_t *free_items;      /* queuing: bos tutamac kuyrugunun deposu, capacity bayt */
    uint8_t *ready_items;     /* queuing: dolu tutamac kuyrugunun deposu, capacity bayt */
    StaticQueue_t *free_ctrl;
    StaticQueue_t *ready_ctrl;
} port_def_t;

extern const port_def_t g_port_defs[PORT_COUNT];

/* ---- Yonetim ---- */
void port_init(void);                                               /* scheduler oncesi */
void port_bind(port_id_t p, TaskHandle_t writer, TaskHandle_t reader);
void port_reset(port_id_t p);                                       /* butun slotlar bos, sayaclar sifir */

/* ---- Queuing port ---- */
port_status_t port_send(port_id_t p, const void *msg);              /* yazan: kopyalar, 0 tick */
port_status_t port_take(port_id_t p, port_handle_t *h);             /* okuyan: en eski mesajin tutamaci */
const void *port_slot(port_id_t p, port_handle_t h);                /* okuyan: alinmis mesaj, yerinde */
void port_release(port_id_t p, port_handle_t h);                    /* okuyan: slotu geri ver (tam bir kez) */
uint32_t port_free_count(port_id_t p);                              /* bos slot sayisi */
QueueHandle_t port_ready_queue(port_id_t p);                        /* yalnizca doluluk okuma ve iz adi icin */

/* ---- Sampling port ---- */
port_status_t port_write(port_id_t p, const void *msg);             /* yazan: ustune yazar */
port_status_t port_read(port_id_t p, void *msg, port_sample_info_t *info); /* okuyan */

void port_get_stats(port_id_t p, port_stats_t *out);

#endif /* PORT_LAYER_H */

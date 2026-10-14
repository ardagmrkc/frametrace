#ifndef QUEUE_EXP_H
#define QUEUE_EXP_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Hafta 3 queue deneyi (odev-03): "Kuyruk dolunca sistemim ne yapiyor?"
 *
 *   Producer (P2) --Message (12 bayt, kopya)--> MessageQueue --> Consumer (P1)
 *
 * Bu A/B/C, Hafta 2'nin oncelik surumleri (main.h g_variant) DEGILDIR:
 *
 *   Surum  Kapasite  Gonderme beklemesi  Degisen tek etken
 *   A      4         0 tick              baslangic
 *   B      16        0 tick              A -> B: kapasite
 *   C      4         20 ms -> tick       A -> C: bekleme politikasi
 *
 * Uc surumde sabit kalanlar: oncelikler (Producer 2, Consumer 1, komutlari
 * isleyen TelemetryTask 3), Message boyutu, tuketicinin 20 ms servis
 * beklemesi (vTaskDelay; CPU yuku degildir) ve yuk tarifi.
 *
 * Yuk tarifi: her kosuda 44 gonderme girisimi.
 *   normal: her girisimden sonra 100 ms bekleme.
 *   yogun : 10 normal, 24 art arda girisim (11..34, aralarinda bekleme yok),
 *           10 normal. 34. girisimden sonra yine 100 ms beklenir: art arda
 *           bolum tam 24 girisimdir.
 *   Son (44.) girisimden sonra beklenmez; kosu, kuyruk bosalip son mesajin
 *   servisi bitince kapanir.
 *
 * Kosu yer istasyonu komutuyla baslar (protocol.h, 'Q'); olcum sirasinda
 * hatta hic satir cikmaz. Kosu bitince kayitlar UartTxTask tarafindan
 * QMSG/QCNT/QEND satirlari olarak dokulur. Ayni anda tek kosu vardir.
 */

#define QEXP_ATTEMPTS        44U
#define QEXP_CAPACITY_SMALL  4U   /* A, C */
#define QEXP_CAPACITY_LARGE  16U  /* B */
#define QEXP_CAPACITY_ARDA   32U  /* Arda: mesaj tablosu ve indeks kuyruklari (arda.h) */
#define QEXP_SEND_WAIT_MS_C  20U  /* C: xQueueSend bekleme suresi */
#define QEXP_SERVICE_MS      20U  /* tuketicinin modellenen servis beklemesi */
#define QEXP_GAP_MS          100U /* normal girisimden sonraki bekleme */
#define QEXP_BURST_FIRST     11U  /* yogun yukte art arda bolumun ilk girisimi */
#define QEXP_BURST_LEN       24U
#define QEXP_BURST_LAST      (QEXP_BURST_FIRST + QEXP_BURST_LEN - 1U) /* 34 */

/* Komuttan sonra ilk girisime kadar beklenen sure: kosuyu onaylayan QRUN
 * satiri bu arada hattan cikar, olcum basladiginda UART sessizdir. */
#define QEXP_SETTLE_MS       50U

/* Tuketicinin alma beklemesi. Kosu icinde iki girisim arasi en fazla
 * QEXP_GAP_MS oldugu icin tuketici kosu ortasinda zaman asimina dusmez;
 * zaman asimi yalnizca Producer bittikten sonra kosunun sonunu bildirir. */
#define QEXP_RX_TIMEOUT_MS   250U

_Static_assert(QEXP_RX_TIMEOUT_MS > QEXP_GAP_MS + QEXP_SETTLE_MS,
               "tuketici kosu ortasinda zaman asimina dusmemeli");
_Static_assert(QEXP_BURST_LAST < QEXP_ATTEMPTS, "art arda bolum kosunun icinde kalmali");

/* Kuyruga giren eleman: deger olarak kopyalanir (ders-03, "Veri sahipligi"). */
typedef struct
{
    uint32_t sequence;     /* 1..QEXP_ATTEMPTS */
    uint32_t generated_us; /* TIM2, gonderme cagrisindan hemen once */
    uint32_t value;        /* kosu numarasi: tuketici mesajin bu kosuya ait oldugunu dogrular */
} qexp_message_t;

_Static_assert(sizeof(qexp_message_t) == 12U, "Message 12 bayt olmali (ders-03)");

typedef enum
{
    QEXP_LOAD_NORMAL = 0,
    QEXP_LOAD_BURST = 1, /* "yogun" */
} qexp_load_t;

/* Yogun yukte art arda bolumdeki girisim mi? (seq 1..QEXP_ATTEMPTS) */
static inline bool qexp_is_burst_attempt(qexp_load_t load, uint32_t seq)
{
    return load == QEXP_LOAD_BURST && seq >= QEXP_BURST_FIRST && seq <= QEXP_BURST_LAST;
}

/* seq. girisimden sonra beklenecek sure (ms). */
static inline uint32_t qexp_gap_after_ms(qexp_load_t load, uint32_t seq)
{
    if (seq >= QEXP_ATTEMPTS)
    {
        return 0U; /* son girisim: kosu kapanisi kuyrugun bosalmasini bekler */
    }
    if (qexp_is_burst_attempt(load, seq) && seq != QEXP_BURST_LAST)
    {
        return 0U;
    }
    return QEXP_GAP_MS;
}

/* Bir kosunun tanimi; QRUN satirinda hatta da cikar. */
typedef struct
{
    uint16_t run;        /* acilistan beri kosu numarasi (1..65535, sonra 1'e doner) */
    char variant;        /* 'A', 'B', 'C'; ek deney: 'E' (Arda2) */
    qexp_load_t load;
    uint8_t capacity;
    uint16_t send_wait_ticks;
    uint16_t service_ms; /* tuketicinin mesaj basina servisi (Arda2: iletim modeli, cerceveye yuvarlanmis) */
} qexp_run_info_t;

/* Ek deney (arda.h): tek surum Arda2 (E), adim 1 + 2 + 3. Yuk tarifi ve mesaj
 * (12 bayt) A/B/C ile aynidir. Adim 1'in tek basina surumu (D) kapali. */
static inline bool qexp_variant_is_arda(char v)
{
    return /* v == 'D' || */ v == 'E';
}

/* Bir mesajin kaydi (doku icin birlestirilmis gorunum). */
typedef struct
{
    bool sent_ok;        /* xQueueSend pdPASS dondu */
    bool received;       /* tuketici aldi ve servisi bitti */
    uint8_t depth;       /* basarili gonderimden hemen sonra kuyruktaki mesaj sayisi */
    uint32_t t_us;       /* uretim zamani, kosunun ilk uretiminden itibaren */
    uint32_t send_us;    /* send_call_us: gonderme cagrisinin duvar saati suresi */
    uint32_t g2r_us;     /* generation_to_receive_us: uretimden servis baslangicina (yalnizca received) */
    uint32_t svc_us;     /* servis baslangicindan bitisine (yalnizca received) */
} qexp_record_t;

typedef struct
{
    uint32_t attempted;
    uint32_t accepted;
    uint32_t failed;
    uint32_t consumed;
    uint32_t queue_max;       /* basarili gonderimden sonra gorulen en yuksek doluluk */
    uint32_t rx_idle_timeout; /* Producer bitmeden tuketicinin alma beklemesi doldu (0 olmali) */
    uint32_t order_error;     /* sira bozuk ya da baska kosuya ait mesaj (0 olmali) */
    uint32_t run_us;          /* ilk uretimden son servis bitisine */
    uint32_t stack_hwm_producer; /* acilistan beri en az bos yigin (word) */
    uint32_t stack_hwm_consumer;
    uint32_t dump_retry;      /* doku istegi UART kuyrugunda yer bulamadi (0 olmali; dokumde yalnizca Arda) */
    uint32_t status_backlog_max; /* Arda: Producer'in durum portunda gordugu en buyuk birikme */
} qexp_summary_t;

/* Scheduler baslamadan: kuyruklar ve gorevler (Arda'nin cerceve gorevi
 * dahil) olusturulur. */
void queue_exp_init(void);

/* Komut argumani (protocol.h CMD_QRUN_*) ile yeni kosu baslatir; yalnizca
 * TelemetryTask cagirir. Onceki kosu bitmemisse (calisiyor ya da dokuluyor)
 * false doner ve info->run calisan kosunun numarasidir. */
bool queue_exp_start(uint8_t cmd_arg, qexp_run_info_t *info);

/* Doku (yalnizca UartTxTask, kosu bittikten sonra). */
void queue_exp_get_info(qexp_run_info_t *out);
bool queue_exp_get_record(uint32_t seq, qexp_record_t *out);
void queue_exp_get_summary(qexp_summary_t *out);

/* Kayitlar dokuldu: yeni kosu kabul edilebilir. */
void queue_exp_dump_done(void);

#endif /* QUEUE_EXP_H */

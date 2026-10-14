#ifndef ARDA_H
#define ARDA_H

/*
 * Arda/yogun: Hafta 3'un ek deneyi (odevin A/B/C matrisinin disinda).
 * Producer, yuk tarifi ve kayitlar A/B/C ile aynidir (queue_exp.c); yalnizca
 * tasima ve tuketici tarafi farklidir.
 *
 * Adimlar, her biri bir oncekinin uzerine eklendi. Tek surum Arda2 ('E',
 * kayit 14) altisinin toplamidir:
 *   Adim 1: 1 ms cerceve gorevi (polling), statik tablo + 1 baytlik indeks,
 *           0 tick bekleme, derleme anli boyut hesabi
 *   Adim 2: servis = 15 baytlik ikili cercevenin iletim suresi (model)
 *   Adim 3: port katmani (queuing + sampling), deadline sayaci (K7),
 *           servis basi = cerceve basi
 *   Adim 4: ikinci kurye = ikinci hat (ARDA_COURIERS 2)
 *   Adim 5: toplu servis, seferde en cok 3 mesaj (ARDA_BATCH_MAX 3)
 *   Adim 6: cerceve tick'ten ayri zamanlayicidan (TIM2), tick kenarindan
 *           yarim cerceve sonra (ARDA_FRAME_PHASE_US, frame_timer.h)
 * Adim 1'in tek basina surumu (Arda, 'D', servis 20 ms) yorum satirinda:
 * queue_exp.c k_variants, command.c, qexp_variant_is_arda.
 *
 * Tasima port katmanindadir (port_layer.h, port_config.c). Gorevler birbirine
 * yalnizca portlarla baglidir:
 *
 *   Producer --port_send--> [PORT_ARDA_MSG: queuing, 32 slot] --> [her 1 ms] cerceve gorevi
 *                                  1) seferi biten kuryeler -> DONE, port_release
 *                                  2) port_take ile portu bosalt -> gelen kutusu
 *                                  3) bos kuryeye sefer ver (servis baslar), deadline denetimi
 *                                  4) durumu yaz
 *   Producer <--port_read-- [PORT_ARDA_STATUS: sampling] <--port_write-- cerceve gorevi
 *
 * Mesajlar portun statik slotlarinda durur; port icindeki kuyruklarda
 * yalnizca 1 baytlik slot tutamaci tasinir. Cerceve gorevi mesaji slotunda
 * okur (port_slot) ve servis bitince tutamaci tam bir kez geri verir.
 *
 * Kurallar (ongorulebilirlik icin):
 *   K1-K4  Slot, tutamac ve durum kurallari port katmanindadir (port_layer.h
 *       P1-P4): statik slotlar, 1 baytlik tutamac, her tutamac her an tek
 *       yerde, durum BOS -> KUYRUKTA -> ALINDI -> BOS; gecersiz tutamac
 *       slota erisim icin kullanilmaz, saglik izleyiciye bildirilir.
 *   K5  Gelen kutusu port kapasitesi kadardir: porttan alinan her tutamac
 *       sigar. Tek kapasite siniri porttur; dolarsa gonderim basarisiz
 *       sayilir (pool_empty = port overflow), sessizce kaybolmaz.
 *   K6  Her sayacin tek yazari vardir (asagidaki arda_stats_t yorumlari).
 *       Kosu sonunda butun tutamaclar portun bos kuyrugunda olmalidir
 *       (pool_leak 0).
 *   K7  Deadline (DL-04): uretimden servis baslangicina en cok
 *       ARDA_DEADLINE_MS. Asan mesaj atilmaz (KAY-01): servis edilir,
 *       sayilir (deadline_miss) ve saglik izleyiciye HM_DEADLINE_MISS olarak
 *       bildirilir (KAY-04). Servis baslangici, seferin basladigi cercevenin
 *       baslangic anidir.
 *
 * Durum portu (sampling): cerceve gorevi her cercevenin sonunda son durumu
 * yazar (arda_status_t); Producer her gonderimden once okur. Durum verisi
 * olay degildir: Producer'in okumadigi guncellemeler atlanir (hata degil);
 * okudugu deger iki cerceveden eskiyse bayattir (cerceve gorevi calismiyor)
 * ve saglik izleyiciye bildirilir. Producer bu adimda degeri yalnizca
 * izlemek icin kullanir (status_backlog_max); gonderme karari degismez.
 *
 * Kuryeler gorev degildir: cerceve gorevinin tuttugu sayaclardir. Sefer bir
 * dis islemi modeller; CPU harcamaz. Seferin suresi kosu basinda secilir
 * (arda_service_t):
 *   Arda  (D, adim 1): ARDA_SERVICE_FIXED, A/B/C ile ayni 20 ms (QEXP_SERVICE_MS).
 *   Arda2 (E, adim 2): ARDA_SERVICE_TX, mesajin ikili cercevede iletim suresi:
 *       senkron (1) + tip (1) + mesaj (12) + CRC (1) = 15 bayt, bayt basina
 *       10 bit (8N1), UART_BAUDRATE'te ~651 us; cerceveye yuvarlanir (1 ms).
 *       Bu bir modeldir: kosu sirasinda hatta hicbir sey gonderilmez (UART
 *       sonuclari tasir ve olcum boyunca sessiz kalir). Yuk tarifi ve mesaj
 *       A/B/C ile ayni; degisen tek etken servis suresi.
 *
 * Kurye = hat (adim 4). Bir sefer bir mesajin hattan iletimidir; tek UART
 * hatti ayni anda tek cerceve tasir. Bu yuzden her kurye ayri bir hatti
 * modeller (kartta bosta USART1/3/6, UART4/5; USART2 yer istasyonunda). Iki
 * hat kapasite icin kullanilir, yedeklilik icin degil: ayni anda iki mesaj
 * iki hatta gider, alici onlari sira numarasiyla (qexp_message_t.sequence)
 * birlestirir. Dagitim en eski mesaji en dusuk numarali bos kuryeye verir;
 * seferler esit sureli oldugu icin servisler basladiklari sirayla biter.
 *
 * Toplu servis (adim 5). Bos kurye, gelen kutusunda bekleyenlerden en cok
 * ARDA_BATCH_MAX tanesini alir; dolmasini beklemez. Yukteki n mesaj ayni
 * hatta arka arkaya gider (DMA), her biri kendi 15 baytlik cercevesinde:
 * mesaj bicimi ve yuk degismez, yalnizca 1 ms'ye yuvarlamada bosa giden
 * cerceve sonu kullanilir. k. mesajin iletimi sefer basindan k x 651 us
 * sonra baslar (servis basi ve deadline bu ana gore); sefer
 * ceil(n x 651 us / 1 ms) cercevede biter (1 mesaj 1, 2-3 mesaj 2 cerceve)
 * ve butun yuk o cercevede teslim edilmis sayilir. Hicbir n icin tek tek
 * seferlerden yavas degildir.
 * Birden cok bos kurye varsa mesajlar onlara sirayla dagitilir (seritleme):
 * ayni anda yola cikan hatlarda servis baslari sira numarasini izler.
 *
 * Adim 1-3: 1 kurye; adim 4: 2 kurye. Adim 1-4: seferde 1 mesaj; adim 5: 3.
 */
#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "queue_exp.h"

#ifndef ARDA_FRAME_TICKS
#define ARDA_FRAME_TICKS 1U /* cerceve periyodu: 1 tick = 1 ms */
#endif
#ifndef ARDA_FRAME_PHASE_US
#define ARDA_FRAME_PHASE_US 500U /* adim 6: cerceve, tick kenarindan bu kadar sonra (frame_timer.h) */
#endif
#ifndef ARDA_COURIERS
#define ARDA_COURIERS 2U    /* adim 4: hat sayisi = ayni anda yolda olabilecek sefer */
#endif
#ifndef ARDA_BATCH_MAX
#define ARDA_BATCH_MAX 3U   /* adim 5: bir seferin tasiyabilecegi en cok mesaj */
#endif
#define ARDA_POOL_LEN  QEXP_CAPACITY_ARDA /* mesaj portunun slotlari = gelen kutusu */
#define ARDA_TRIP_MS   QEXP_SERVICE_MS
#define ARDA_FRAME_US  (ARDA_FRAME_TICKS * (1000000U / configTICK_RATE_HZ))
#define ARDA_DEADLINE_MS 20U /* K7, DL-04: uretimden servis baslangicina */
#define ARDA_DEADLINE_US (ARDA_DEADLINE_MS * 1000U)
#define ARDA_STATUS_REFRESH_US (2U * ARDA_FRAME_US) /* durum iki cerceveden eskiyse bayat */

_Static_assert(ARDA_FRAME_TICKS >= 1U, "cerceve en az 1 tick");
_Static_assert(ARDA_COURIERS >= 1U && ARDA_BATCH_MAX >= 1U, "en az bir kurye, seferde en az bir mesaj");
_Static_assert(ARDA_COURIERS <= 5U, "her kurye ayri bir UART: kartta bosta 5 UART var");
_Static_assert(ARDA_BATCH_MAX <= ARDA_POOL_LEN, "sefer porttan buyuk olamaz");

/*
 * Port kapasitesi hesabi. Ayni anda kullanimdaki tutamac = kabul edilmis ama
 * servisi bitmemis mesaj. Yuk tarifi queue_exp.h'dedir:
 *   (a) Art arda bolumden once sistem bostur: iki normal girisim arasi
 *       QEXP_GAP_MS (100 ms), bir seferden (ARDA_TRIP_MS, 20 ms) uzundur;
 *       her normal mesaj bir sonraki gelmeden servis edilir.
 *   (b) Art arda bolum QEXP_BURST_LEN (24) mesaji ~0,12 ms'de getirir; bu
 *       surede hicbir sefer bitmez. Kullanimdaki tutamac en cok 24 olur.
 *   (c) Sonra her QEXP_GAP_MS'de 1 mesaj gelir, en az
 *       (QEXP_GAP_MS / ARDA_TRIP_MS) x ARDA_COURIERS = 10 mesaj servis
 *       edilir (toplu servis hesaba katilmadan): birikme yalnizca azalir.
 *   Sonuc: en cok QEXP_BURST_LEN = 24 tutamac. Port 24 + ARDA_POOL_MARGIN
 *   (8, olcum disi pay) = 32. Olcum: inbox_max 24 (simulator).
 * Yuk tarifi, sefer suresi ya da port kapasitesi degisip hesap tutmazsa derleme durur.
 */
#define ARDA_POOL_MARGIN 8U
_Static_assert(QEXP_GAP_MS >= ARDA_TRIP_MS, "(a) art arda bolumden once sistem bos olmali");
_Static_assert((QEXP_GAP_MS / ARDA_TRIP_MS) * ARDA_COURIERS > 1U,
               "(c) art arda bolumden sonra servis gelisten hizli olmali");
_Static_assert(ARDA_POOL_LEN >= QEXP_BURST_LEN + ARDA_POOL_MARGIN, "(b) port art arda bolumu ve payi almali");

/* PORT_ARDA_STATUS (sampling) icerigi: cerceve gorevi her cercevenin
 * sonunda yazar. */
typedef struct
{
    uint32_t frame;          /* bu kosudaki cerceve sayisi */
    uint32_t backlog;        /* servis bekleyen mesaj (gelen kutusu) */
    uint32_t oldest_wait_us; /* gelen kutusundaki en eski mesajin uretimden beri yasi; 0: bos */
    uint32_t deadline_miss;  /* bu kosuda deadline'i kacan mesaj */
} arda_status_t;

/* Kosu sayaclari: arda_run_begin sifirlar, dokumde QCNT satiri olarak gider.
 * Port ve saglik sayaclari (pool_empty, pool_*_error, status_*) port_layer.h ve
 * health.h'dedir. */
typedef struct
{
    /* Cerceve gorevi yazar: */
    uint32_t frames;            /* calisan cerceve sayisi */
    uint32_t frame_miss;        /* bir bekleyiste birden cok cerceve geldi: cerceve kacti (0 olmali) */
    uint32_t frame_dt_min_us;   /* ardisik iki cerceve baslangici arasi, en kisa */
    uint32_t frame_dt_max_us;   /* en uzun; ideal ARDA_FRAME_TICKS x 1000 us */
    uint32_t frame_work_max_us; /* bir cercevenin isi (bosaltma + sefer), en uzun */
    uint32_t intake_max_us;     /* uretimden giris kuyrugundan alinmaya, en uzun */
    uint32_t inbox_max;         /* gelen kutusunda ayni anda bekleyen en cok mesaj */
    uint32_t pool_leak;         /* kosu sonunda portun bos kuyruguna donmemis tutamac (0 olmali) */
    uint32_t deadline_miss;     /* K7: servisi deadline'dan gec baslayan mesaj */
    uint32_t courier_busy_max;  /* ayni anda yolda olan en cok kurye (hat); en cok ARDA_COURIERS */
    uint32_t batch_max;         /* bir seferdeki en cok mesaj; en cok ARDA_BATCH_MAX */
    uint32_t frame_phase_min_us; /* cerceve basinda son tick kenarindan gecen sure, en kisa */
    uint32_t frame_phase_max_us; /* en uzun; ideal ARDA_FRAME_PHASE_US + kesme gecikmesi */
} arda_stats_t;

/* Seferin (servisin) suresi. Secilen tek model ARDA_SERVICE_TX (Arda2);
 * ARDA_SERVICE_FIXED yalnizca adim 1'in kapali surumu icindi, sabit sefer
 * suresi (20 ms) boyut hesabinin ihtiyatli ust siniri olarak kalir. */
typedef enum
{
    ARDA_SERVICE_FIXED = 0, /* Arda:  QEXP_SERVICE_MS (20 ms), A/B/C ile ayni */
    ARDA_SERVICE_TX,        /* Arda2: 15 baytlik ikili cercevenin iletim suresi (model) */
} arda_service_t;

/* Scheduler baslamadan cerceve gorevi olusturulur (queue_exp_init cagirir;
 * portlar ondan once port_init ile kurulur). */
void arda_init(void);

/* Kosu basi (queue_exp_start, kosu IDLE iken; Arda'nin hicbir parcasi
 * calismiyor): iki port ve saglik sayaclari sifirlanir (butun tutamaclar
 * bos), Arda sayaclari sifira, sefer suresi secilen servis modeline. */
void arda_run_begin(arda_service_t service);

/* Servis modelinin mesaj basina suresi (ms, cerceveye yuvarlanmis); QRUN
 * satirinda servis_ms olarak gider. */
uint16_t arda_service_ms(arda_service_t service);

/* Kosunun tuketici gorevi: Producer kosu basinda buna bildirim verir. */
TaskHandle_t arda_task(void);

/* Son Arda kosusunun sayaclari (doku, kosu bittikten sonra). */
void arda_get_stats(arda_stats_t *out);

#endif /* ARDA_H */

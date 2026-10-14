/* queue_exp.c icin uctan uca test: gercek FreeRTOS cekirdegi (tasks.c,
 * queue.c, list.c) Windows simulator portunda (MSVC-MingW) calisir;
 * queue_exp.c firmware'deki dosyanin kendisidir, degistirilmez.
 *
 * Derleme (depo kokunden, MinGW gcc):
 *   K=firmware/Middlewares/FreeRTOS-Kernel
 *   gcc -std=gnu11 -O1 -Wall -Wextra -I tests/host/sim -I firmware/Core/Inc
 *       -I $K/include -I $K/portable/MSVC-MingW
 *       tests/host/sim/test_queue_exp_sim.c firmware/Core/Src/queue_exp.c
 *       firmware/Core/Src/arda.c firmware/Core/Src/port_layer.c firmware/Core/Src/port_config.c
 *       firmware/Core/Src/health.c firmware/Core/Src/protocol.c $K/tasks.c $K/queue.c $K/list.c
 *       $K/portable/MSVC-MingW/port.c -lwinmm -o test_queue_exp_sim
 *   ./test_queue_exp_sim        (~35 s: yedi kosu gercek zamanli)
 *   ./test_queue_exp_sim DIZIN  ayrica her kosunun hatta gidecek baytlarini
 *                               (QRUN + QMSG/QCNT/QEND, 64 baytlik satirlar)
 *                               DIZIN/q_<surum>_<yuk>.bin dosyasina yazar;
 *                               analysis/queue_experiment.py kartsiz denenebilir.
 *
 * TelemetryTask rolundeki surucu gorev (P3) alti kosuyu sirayla baslatir
 * (A/B/C x normal/yogun) ve ardindan Arda2/yogun kosusunu (adim 1-6)
 * (ek deney, arda.c), dokum istegini (MSG_CMD_QDUMP) bekler ve kayitlari
 * dogrular:
 *   - attempted = 44 = accepted + failed, consumed = accepted, sira hatasi
 *     ve tuketici zaman asimi yok
 *   - kabul edilen her mesaj alindi, basarisiz gonderimin gecikmesi yok;
 *     her QMSG satiri 64 bayta sigar
 *   - kosu ya da doku surerken yeni kosu reddedilir (QBUSY)
 *   - normal yukte hic kayip yok; A/yogun 15..34, B/yogun 27..34 kaybeder
 *     (Producer P2 > Consumer P1: 0 tick ile art arda gonderirken tuketici
 *     araya giremez)
 *   - C/yogun: kayiplar yalnizca art arda bolumde ve her biri tam bekleme
 *     suresi kadar (20 ms) bloklanmis gonderimdir
 *   - Arda2/yogun: hic kayip yok, gonderim hic bloklanmaz;
 *     mesaj portu dolmaz, her tutamac tam bir kez geri doner; art arda bolum
 *     gelen kutusunda birikir. Arda2'de servis ~1 ms (15 bayt iletim modeli).
 *     Deadline sayaci (saglik izleyici dahil) kayitlardaki g2r > 20 ms
 *     sayisina esit; durum portu (sampling) her cercevede yazilir, her
 *     girisimden once okunur. Iki kurye (hat) art arda bolumde ayni anda
 *     yolda olur, seferler dolu (3 mesaj) gider; servis baslari sira
 *     numarasini izler. Cerceve saati (frame_timer.h) burada tick'e
 *     baglidir (faz 0): adim 6'nin etkisi yalnizca kartta olculur.
 * Sure olcumleri Windows zamanlamasina baglidir; kartin sonucu yerine
 * gecmez, yalnizca mantigi dogrular. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue_exp.h"
#include "arda.h"
#include "port_layer.h"
#include "health.h"
#include "frame_timer.h"
#include "protocol.h"
#include "tx_queue.h"

static int g_fail;
#define CHECK(c, msg) do { if (!(c)) { printf("  HATA: %s (satir %d)\n", msg, __LINE__); g_fail++; } } while (0)

static TaskHandle_t s_driver;
static StackType_t s_driver_stack[configMINIMAL_STACK_SIZE * 4U];
static StaticTask_t s_driver_tcb;
static volatile uint32_t s_dump_requests;

void vAssertCalled(const char *file, unsigned long line)
{
    printf("configASSERT: %s:%lu\n", file, line);
    fflush(stdout);
    exit(2);
}

uint32_t timestamp_now_us(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (freq.QuadPart == 0)
    {
        QueryPerformanceFrequency(&freq);
    }
    QueryPerformanceCounter(&now);
    const long long sec = now.QuadPart / freq.QuadPart;
    const long long rem = now.QuadPart % freq.QuadPart;
    return (uint32_t)(sec * 1000000LL + (rem * 1000000LL) / freq.QuadPart);
}

/* frame_timer.c yerine: Windows simulatorunde tick'ten ince saat yok; cerceve
 * tick'e baglidir (faz 0). Gec kalinirsa gecen tick'ler kacirilmis cerceve
 * olarak doner (kartta TIM2 kesmesinin yaptigi gibi). */
static TickType_t s_ft_next;

void frame_timer_init(void) {}

void frame_timer_start(TaskHandle_t task)
{
    (void)task;
    s_ft_next = xTaskGetTickCount() + 1U;
}

uint32_t frame_timer_wait(void)
{
    const TickType_t now = xTaskGetTickCount();
    if ((int32_t)(s_ft_next - now) > 0)
    {
        vTaskDelay(s_ft_next - now);
    }
    const uint32_t n = 1U + (uint32_t)(xTaskGetTickCount() - s_ft_next);
    s_ft_next = xTaskGetTickCount() + 1U;
    return n;
}

void frame_timer_stop(void) {}
uint32_t frame_timer_phase_us(void) { return 0U; }
uint32_t frame_timer_since_tick_us(void) { return 0U; }
void frame_timer_isr(void) {}

/* tx_queue.c yerine: Consumer kosu sonunda dokum ister. */
bool tx_queue_send(const tx_message_t *msg, TickType_t wait)
{
    (void)wait;
    if (msg->type == MSG_CMD_QDUMP)
    {
        s_dump_requests++;
        xTaskNotifyGive(s_driver);
    }
    return true;
}

typedef struct
{
    uint8_t arg;
    char variant;
    bool burst;
} case_t;

static const case_t k_cases[] = {
    { 0x00, 'A', false }, { 0x01, 'B', false }, { 0x02, 'C', false },
    { 0x10, 'A', true },  { 0x11, 'B', true },  { 0x12, 'C', true },
    /* { 0x13, 'D', true },  Arda/yogun, adim 1 tek basina: kapali (arda.h) */
    { 0x14, 'E', true }, /* Arda2/yogun: adim 1-6 */
};

static const char *s_dump_dir; /* NULL: bayt dosyasi yazilmaz */

/* uart_tx_task.c queue_run_dump ile ayni eslesme. */
static tx_message_t qmsg_of(uint16_t run, uint32_t seq, const qexp_record_t *r)
{
    tx_message_t m = { .type = MSG_QMSG };
    m.u.qmsg.run = run;
    m.u.qmsg.seq = (uint8_t)seq;
    m.u.qmsg.sent_ok = r->sent_ok;
    m.u.qmsg.received = r->received;
    m.u.qmsg.depth = r->depth;
    m.u.qmsg.t_us = r->t_us;
    m.u.qmsg.send_us = r->send_us;
    m.u.qmsg.g2r_us = r->g2r_us;
    m.u.qmsg.svc_us = r->svc_us;
    return m;
}

static bool qmsg_fits(uint16_t run, uint32_t seq, const qexp_record_t *r)
{
    const tx_message_t m = qmsg_of(run, seq, r);
    uint8_t buf[PROTOCOL_FRAME_SIZE];
    return protocol_encode(&m, 0U, buf) == PROTOCOL_FRAME_SIZE;
}

static void put_frame(FILE *f, const tx_message_t *m)
{
    uint8_t buf[PROTOCOL_FRAME_SIZE];
    const size_t n = protocol_encode(m, 0U, buf);
    CHECK(n == PROTOCOL_FRAME_SIZE, "satir kodlanmali");
    fwrite(buf, 1, n, f);
}

/* Kartin hatta gonderecegi baytlar: telemetry_task.c start_queue_run (QRUN)
 * ve uart_tx_task.c queue_run_dump (QMSG/QCNT/QEND) ile ayni sira. */
static void write_wire(const qexp_run_info_t *info, const qexp_summary_t *s)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/q_%c_%s.bin", s_dump_dir, info->variant,
             info->load == QEXP_LOAD_BURST ? "yogun" : "normal");
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL, "bayt dosyasi acilmali");
    if (f == NULL)
    {
        return;
    }
    tx_message_t m = { .type = MSG_QRUN };
    m.u.qrun.run = info->run;
    m.u.qrun.variant = info->variant;
    m.u.qrun.burst = (info->load == QEXP_LOAD_BURST);
    m.u.qrun.capacity = info->capacity;
    m.u.qrun.wait_ticks = info->send_wait_ticks;
    m.u.qrun.service_ms = info->service_ms;
    m.u.qrun.gap_ms = QEXP_GAP_MS;
    m.u.qrun.trace = true;
    put_frame(f, &m);
    for (uint32_t seq = 1; seq <= QEXP_ATTEMPTS; seq++)
    {
        qexp_record_t r;
        (void)queue_exp_get_record(seq, &r);
        m = qmsg_of(info->run, seq, &r);
        put_frame(f, &m);
    }
    uint32_t values[QCNT_COUNT] = {
        s->attempted, s->accepted, s->failed, s->consumed, s->queue_max, s->rx_idle_timeout,
        s->order_error, s->run_us, s->stack_hwm_producer, s->stack_hwm_consumer,
    };
    int count = QCNT_BASE_COUNT; /* uart_tx_task.c: Arda sayaclari yalnizca Arda kosusunda */
    if (qexp_variant_is_arda(info->variant))
    {
        arda_stats_t a;
        port_stats_t pm;
        port_stats_t ps;
        arda_get_stats(&a);
        port_get_stats(PORT_ARDA_MSG, &pm);
        port_get_stats(PORT_ARDA_STATUS, &ps);
        values[QCNT_FRAMES] = a.frames;
        values[QCNT_FRAME_MISS] = a.frame_miss;
        values[QCNT_FRAME_DT_MIN_US] = a.frame_dt_min_us;
        values[QCNT_FRAME_DT_MAX_US] = a.frame_dt_max_us;
        values[QCNT_FRAME_WORK_MAX_US] = a.frame_work_max_us;
        values[QCNT_INTAKE_MAX_US] = a.intake_max_us;
        values[QCNT_INBOX_MAX] = a.inbox_max;
        values[QCNT_POOL_EMPTY] = pm.overflow;
        values[QCNT_POOL_ALLOC_ERROR] = hm_count(HM_PORT_ALLOC_ERROR);
        values[QCNT_POOL_FREE_ERROR] = hm_count(HM_PORT_FREE_ERROR);
        values[QCNT_POOL_LEAK] = a.pool_leak;
        values[QCNT_DUMP_RETRY] = s->dump_retry;
        values[QCNT_DEADLINE_MISS] = a.deadline_miss;
        values[QCNT_STATUS_WRITES] = ps.writes;
        values[QCNT_STATUS_READS] = ps.reads;
        values[QCNT_STATUS_STALE] = ps.stale_reads;
        values[QCNT_STATUS_MISSED] = ps.missed_updates;
        values[QCNT_STATUS_AGE_MAX_US] = ps.age_max_us;
        values[QCNT_STATUS_BACKLOG_MAX] = s->status_backlog_max;
        values[QCNT_COURIERS] = ARDA_COURIERS;
        values[QCNT_COURIER_BUSY_MAX] = a.courier_busy_max;
        values[QCNT_BATCH] = ARDA_BATCH_MAX;
        values[QCNT_BATCH_MAX] = a.batch_max;
        values[QCNT_FRAME_PHASE_US] = frame_timer_phase_us();
        values[QCNT_FRAME_PHASE_MIN_US] = a.frame_phase_min_us;
        values[QCNT_FRAME_PHASE_MAX_US] = a.frame_phase_max_us;
        count = QCNT_COUNT;
    }
    for (int id = 0; id < count; id++)
    {
        m = (tx_message_t){ .type = MSG_QCNT };
        m.u.qcnt.run = info->run;
        m.u.qcnt.id = (qcnt_id_t)id;
        m.u.qcnt.value = values[id];
        put_frame(f, &m);
    }
    m = (tx_message_t){ .type = MSG_QEND };
    m.u.qend.run = info->run;
    m.u.qend.records = QEXP_ATTEMPTS;
    put_frame(f, &m);
    fclose(f);
}

static void run_case(const case_t *c, uint16_t expect_run)
{
    qexp_run_info_t info;
    qexp_run_info_t busy;
    const uint32_t dumps_before = s_dump_requests;

    CHECK(queue_exp_start(c->arg, &info), "bos kart kosuyu kabul etmeli");
    CHECK(info.run == expect_run, "kosu numarasi sirayla artmali");
    CHECK(info.variant == c->variant, "surum");
    CHECK(info.load == (c->burst ? QEXP_LOAD_BURST : QEXP_LOAD_NORMAL), "yuk");
    CHECK(info.capacity == (c->variant == 'B'                ? 16U
                            : qexp_variant_is_arda(c->variant) ? QEXP_CAPACITY_ARDA
                                                                : 4U),
          "kapasite");
    CHECK(info.service_ms == (c->variant == 'E' ? 1U : QEXP_SERVICE_MS), "servis: Arda2 1 ms, digerleri 20 ms");
    CHECK(info.send_wait_ticks == (c->variant == 'C' ? 20U : 0U), "gonderme beklemesi");
    CHECK(!queue_exp_start(c->arg, &busy) && busy.run == info.run, "kosu surerken QBUSY");

    const uint32_t got = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(30000));
    CHECK(got == 1U && s_dump_requests == dumps_before + 1U, "kosu bitmeli ve tek dokum istemeli");
    if (got == 0U)
    {
        printf("  kosu 30 s icinde bitmedi\n");
        return;
    }
    CHECK(!queue_exp_start(c->arg, &busy), "doku bitmeden QBUSY");

    qexp_summary_t s;
    queue_exp_get_summary(&s);
    CHECK(s.attempted == QEXP_ATTEMPTS, "attempted = 44");
    CHECK(s.accepted + s.failed == s.attempted, "attempted = accepted + failed");
    CHECK(s.consumed == s.accepted, "accepted = consumed");
    CHECK(s.order_error == 0U, "sira hatasi yok");
    CHECK(s.rx_idle_timeout == 0U, "tuketici kosu ortasinda zaman asimina dusmemeli");

    char fails[160] = "";
    size_t fl = 0;
    uint32_t prev_t = 0, n_fail = 0, svc_min = UINT32_MAX, svc_max = 0, g2r_max = 0;
    uint32_t send_ok_max = 0, send_fail_min = UINT32_MAX, burst_fail_outside = 0, n_late = 0;
    uint32_t prev_start = 0, start_order_error = 0; /* servis basi = t_us + g2r_us */
    char late[160] = "";
    size_t ll = 0;
    for (uint32_t seq = 1; seq <= QEXP_ATTEMPTS; seq++)
    {
        qexp_record_t r;
        CHECK(queue_exp_get_record(seq, &r), "kayit okunmali");
        CHECK(r.sent_ok == r.received, "kabul edilen alinmali, basarisiz alinmamali");
        CHECK(r.t_us >= prev_t, "uretim zamani artan");
        CHECK(qmsg_fits(info.run, seq, &r), "QMSG 64 bayta sigmali");
        prev_t = r.t_us;
        if (r.sent_ok)
        {
            /* Arda: cerceve gorevi (P5) gonderim ile doluluk okumasi arasinda
             * bir tick'e denk gelirse kuyrugu bosaltmis olabilir (0). */
            CHECK((r.depth >= 1U || qexp_variant_is_arda(c->variant)) && r.depth <= info.capacity, "doluluk 1..kapasite");
            svc_min = (r.svc_us < svc_min) ? r.svc_us : svc_min;
            svc_max = (r.svc_us > svc_max) ? r.svc_us : svc_max;
            g2r_max = (r.g2r_us > g2r_max) ? r.g2r_us : g2r_max;
            send_ok_max = (r.send_us > send_ok_max) ? r.send_us : send_ok_max;
            if (r.t_us + r.g2r_us < prev_start)
            {
                start_order_error++;
            }
            prev_start = r.t_us + r.g2r_us;
            if (r.g2r_us > ARDA_DEADLINE_US)
            {
                n_late++;
                ll += (size_t)snprintf(late + ll, sizeof(late) - ll, "%s%u", n_late > 1 ? "," : "",
                                       (unsigned)seq);
            }
        }
        else
        {
            CHECK(r.g2r_us == 0U && r.svc_us == 0U, "basarisiz gonderimin gecikmesi yok");
            n_fail++;
            send_fail_min = (r.send_us < send_fail_min) ? r.send_us : send_fail_min;
            if (!qexp_is_burst_attempt(info.load, seq))
            {
                burst_fail_outside++;
            }
            fl += (size_t)snprintf(fails + fl, sizeof(fails) - fl, "%s%u", n_fail > 1 ? "," : "",
                                   (unsigned)seq);
        }
    }
    CHECK(n_fail == s.failed, "QMSG fail sayisi = failed");
    if (c->variant == 'E')
    {
        /* 15 bayt iletim ~651 us, 1 ms'lik cerceveye yuvarlanir. Ust sinir
         * Windows zamanlamasinin sicramalarina pay birakir. */
        CHECK(svc_min >= 500U && svc_max <= 20000U, "servis ~1 ms (Arda2)");
    }
    else
    {
        CHECK(svc_min >= 15000U && svc_max <= 40000U, "servis ~20 ms");
    }

    if (!c->burst)
    {
        CHECK(s.failed == 0U && s.queue_max == 1U, "normal yukte kayip yok, kuyrukta en fazla 1");
    }
    else if (c->variant == 'A')
    {
        CHECK(s.accepted == 24U && s.failed == 20U, "A/yogun: 24 kabul, 20 kayip");
        CHECK(strcmp(fails, "15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34") == 0,
              "A/yogun: 15..34 kaybolur");
        CHECK(s.queue_max == 4U, "A/yogun: kuyruk dolar (4)");
    }
    else if (c->variant == 'B')
    {
        CHECK(s.accepted == 36U && s.failed == 8U, "B/yogun: 36 kabul, 8 kayip");
        CHECK(strcmp(fails, "27,28,29,30,31,32,33,34") == 0, "B/yogun: 27..34 kaybolur");
        CHECK(s.queue_max == 16U, "B/yogun: kuyruk dolar (16)");
    }
    else if (qexp_variant_is_arda(c->variant))
    {
        arda_stats_t a;
        port_stats_t pm;
        port_stats_t ps;
        arda_get_stats(&a);
        port_get_stats(PORT_ARDA_MSG, &pm);
        port_get_stats(PORT_ARDA_STATUS, &ps);
        CHECK(s.accepted == QEXP_ATTEMPTS && s.failed == 0U, "Arda/yogun: 44 kabul, kayip yok");
        CHECK(pm.sent == QEXP_ATTEMPTS && pm.received == QEXP_ATTEMPTS, "Arda/yogun: port 44 gonderdi, 44 alindi");
        CHECK(pm.overflow == 0U && hm_count(HM_PORT_OVERFLOW) == 0U && hm_count(HM_PORT_ALLOC_ERROR) == 0U,
              "Arda/yogun: port hic dolmaz, tutamac hatasi yok");
        CHECK(hm_count(HM_PORT_FREE_ERROR) == 0U && a.pool_leak == 0U, "Arda/yogun: her tutamac tam bir kez geri doner");
        CHECK(s.dump_retry == 0U, "Arda/yogun: doku istegi beklemeden kondu");
        CHECK(a.inbox_max >= 20U && a.inbox_max <= ARDA_POOL_LEN, "Arda/yogun: art arda bolum gelen kutusunda birikir");
        CHECK(a.frames > 0U && a.frame_dt_min_us > 0U, "Arda: cerceveler calisti");
        CHECK(a.deadline_miss == n_late && hm_count(HM_DEADLINE_MISS) == n_late,
              "Arda: deadline sayaci = kayitlarda g2r > 20 ms olan mesaj");
        CHECK(ps.writes == a.frames, "durum portu: her cercevede bir yazma");
        CHECK(ps.reads == QEXP_ATTEMPTS, "durum portu: her girisimden once bir okuma");
        CHECK(ps.stale_reads == hm_count(HM_STALE_READ), "durum portu: bayat okuma saglik izleyicide");
        CHECK(ps.missed_updates < ps.writes, "durum portu: atlanan < yazilan");
        CHECK(a.courier_busy_max == ARDA_COURIERS, "art arda bolumde butun kuryeler (hatlar) ayni anda yolda");
        CHECK(a.batch_max == ARDA_BATCH_MAX, "art arda bolumde seferler dolu gider (toplu servis)");
        CHECK(start_order_error == 0U, "servis baslari sira numarasini izler");
    }
    else
    {
        CHECK(burst_fail_outside == 0U, "C/yogun: kayip yalnizca art arda bolumde");
        CHECK(s.failed < 20U, "C/yogun: A'dan az kayip");
        CHECK(n_fail == 0U || send_fail_min >= (QEXP_SEND_WAIT_MS_C - 1U) * 1000U,
              "C/yogun: kayip ancak tam bekleme suresinden sonra");
        CHECK(s.queue_max == 4U, "C/yogun: kuyruk dolar (4)");
    }
    if (c->variant != 'C')
    {
        CHECK(send_ok_max < 5000U, "A/B: gonderme hic bloklamaz");
    }

    printf("  %u %c/%-6s kabul %2u kayip %2u q_max %2u  send_ok_max %6.2f ms  g2r_max %7.2f ms  "
           "servis %5.2f..%5.2f ms  kosu %7.1f ms\n",
           (unsigned)info.run, info.variant, c->burst ? "yogun" : "normal", (unsigned)s.accepted,
           (unsigned)s.failed, (unsigned)s.queue_max, send_ok_max / 1000.0, g2r_max / 1000.0,
           svc_min / 1000.0, svc_max / 1000.0, s.run_us / 1000.0);
    if (n_fail > 0U)
    {
        printf("           kaybolan: %s\n", fails);
    }
    if (qexp_variant_is_arda(c->variant))
    {
        /* Cerceve araligi Windows zamanlamasina baglidir; kartin degerleri
         * dokumdeki frame_dt_* sayaclarindadir. */
        arda_stats_t a;
        port_stats_t pm;
        port_stats_t ps;
        arda_get_stats(&a);
        port_get_stats(PORT_ARDA_MSG, &pm);
        port_get_stats(PORT_ARDA_STATUS, &ps);
        printf("           cerceve %u, kacirilan %u, aralik %u..%u us, is en cok %u us | "
               "giris en gec %u us, gelen kutusu en cok %u | port dolu %u, hata %u/%u, sizinti %u\n",
               (unsigned)a.frames, (unsigned)a.frame_miss, (unsigned)a.frame_dt_min_us,
               (unsigned)a.frame_dt_max_us, (unsigned)a.frame_work_max_us, (unsigned)a.intake_max_us,
               (unsigned)a.inbox_max, (unsigned)pm.overflow, (unsigned)hm_count(HM_PORT_ALLOC_ERROR),
               (unsigned)hm_count(HM_PORT_FREE_ERROR), (unsigned)a.pool_leak);
        printf("           deadline (%u ms) kacan %u: %s | durum portu: yazma %u, okuma %u, atlanan %u, "
               "bayat %u, yas en cok %u us, Producer'in gordugu birikme en cok %u\n",
               (unsigned)ARDA_DEADLINE_MS, (unsigned)a.deadline_miss, a.deadline_miss ? late : "-",
               (unsigned)ps.writes, (unsigned)ps.reads, (unsigned)ps.missed_updates, (unsigned)ps.stale_reads,
               (unsigned)ps.age_max_us, (unsigned)s.status_backlog_max);
        printf("           kurye (hat) %u, ayni anda yolda en cok %u | seferde en cok %u, olculen %u | "
               "servis basi sira hatasi %u\n", (unsigned)ARDA_COURIERS, (unsigned)a.courier_busy_max,
               (unsigned)ARDA_BATCH_MAX, (unsigned)a.batch_max, (unsigned)start_order_error);
    }
    if (s_dump_dir != NULL)
    {
        write_wire(&info, &s);
    }
    queue_exp_dump_done();
}

static void driver_task(void *arg)
{
    (void)arg;
    printf("queue_exp.c, FreeRTOS Windows simulatoru\n");
    for (size_t i = 0; i < sizeof(k_cases) / sizeof(k_cases[0]); i++)
    {
        run_case(&k_cases[i], (uint16_t)(i + 1U));
    }
    printf("\n%s (%d hata)\n", g_fail == 0 ? "TUM TESTLER GECTI" : "TESTLER BASARISIZ", g_fail);
    fflush(stdout);
    exit(g_fail == 0 ? 0 : 1);
}

int main(int argc, char **argv)
{
    s_dump_dir = (argc > 1) ? argv[1] : NULL;
    queue_exp_init();
    /* P3: firmware'de komutu isleyen TelemetryTask'in onceligi. */
    s_driver = xTaskCreateStatic(driver_task, "Driver", configMINIMAL_STACK_SIZE * 4U, NULL,
                                 tskIDLE_PRIORITY + 3U, s_driver_stack, &s_driver_tcb);
    vTaskStartScheduler();
    return 3;
}

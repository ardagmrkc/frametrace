#include "arda.h"
#include "queue_exp_core.h"
#include "port_layer.h"
#include "health.h"
#include "frame_timer.h"
#include "mem_layout.h"
#include "main.h"
#include "timestamp.h"
#include "app_trace.h"
#include "task.h"
#include <string.h>

/*
 * Cerceve gorevi (arda.h, kurallar K1..K7). Cerceveyi frame_timer verir
 * (kartta TIM2, tick kenarindan ARDA_FRAME_PHASE_US sonra; adim 6). Seferler
 * cerceve sayisiyla olculur. Her cercevede sira sabittir:
 *   1) complete_trips: suresi dolan seferler biter (DONE), tutamaclar porta geri verilir
 *   2) intake:         mesaj portu 0 tick ile bosaltilir -> gelen kutusu
 *   3) dispatch:       bos kuryeler gelen kutusundan yuk alip yola cikar;
 *                      deadline denetimi (K7)
 *   4) write_status:   durum portuna son durum yazilir
 * Ayni cercevede donen kurye, bekleyen is varsa o cercevede yeniden cikar.
 *
 * Kayit anlamlari (queue_exp_core.h): g2r = uretimden mesajin servis
 * baslangicina (gelen kutusunda bekleme dahil), servis = o andan seferin
 * bittigi cerceveye. Seferin baslangici, yola ciktigi cercevenin baslangic
 * anidir (cerceve isi bundan sonra yapilir); seferdeki k. mesajin servisi
 * bundan k x mesaj suresi sonra baslar (arda.h, toplu servis). W3 RECEIVE
 * izi tutamacin porttan alindigi ani gosterir (lat = intake).
 */

/* Servis modelleri (arda.h arda_service_t). Sefer cerceve sinirinda baslar
 * ve biter: suresi ARDA_FRAME_TICKS'in kati olmali, yoksa bir sonraki
 * cerceveye yuvarlanir ve servis farkinda olmadan uzar. */
#define ARDA_TRIP_TICKS_FIXED pdMS_TO_TICKS(ARDA_TRIP_MS)

/* Arda2: senkron (1) + tip (1) + mesaj + CRC (1); bayt basina 10 bit (8N1). */
#define ARDA_TX_FRAME_BYTES (1U + 1U + sizeof(qexp_message_t) + 1U)
#define ARDA_TX_US          ((ARDA_TX_FRAME_BYTES * 10U * 1000000U + UART_BAUDRATE - 1U) / UART_BAUDRATE)
#define ARDA_TICK_US        (1000000U / configTICK_RATE_HZ)
/* Iletim suresi cerceveye yukari yuvarlanir (en az bir cerceve). */
#define ARDA_TRIP_TICKS_TX  (((ARDA_TX_US + ARDA_FRAME_US - 1U) / ARDA_FRAME_US) * ARDA_FRAME_TICKS)

_Static_assert(ARDA_TX_FRAME_BYTES == 15U, "Arda2 cercevesi: senkron + tip + 12 bayt mesaj + CRC");
_Static_assert(ARDA_TRIP_TICKS_FIXED % ARDA_FRAME_TICKS == 0U, "sabit sefer cerceveye tam bolunmeli");
_Static_assert(ARDA_TRIP_TICKS_TX >= ARDA_FRAME_TICKS && ARDA_TRIP_TICKS_TX <= ARDA_TRIP_TICKS_FIXED,
               "iletim seferi en az bir cerceve, en cok sabit sefer kadar");

/* Bu kosuda bir mesajin hatti mesgul ettigi sure: Arda2'de iletim (651 us). */
static uint32_t s_msg_us = ARDA_TRIP_MS * 1000U;

/* Kurye = hat (arda.h, adim 4). */
typedef struct
{
    port_handle_t load[ARDA_BATCH_MAX];
    uint32_t n;            /* yukteki mesaj sayisi; 0: kurye bosta */
    uint32_t start_frame;  /* seferin basladigi cerceve */
    uint32_t trip_frames;  /* seferin suresi: ceil(n x s_msg_us / cerceve) cerceve */
    uint32_t start_us;     /* o cercevenin baslangic ani */
} courier_t;

/* Cerceve gorevinin ic durumu CCM'de (mem_layout.h): yalnizca islemci
 * erisir. Mesajlarin kendisi port slotlarinda, ana SRAM'de kalir. */

/* qexp_consumer_take sonucu, tutamac basina; cerceve gorevi yazar ve okur. */
CCM_DATA static bool s_valid[ARDA_POOL_LEN];

/* Cerceve gorevinin: gelen kutusu (halka, en eski s_inbox[s_inbox_head]) ve kuryeler. */
CCM_DATA static port_handle_t s_inbox[ARDA_POOL_LEN];
CCM_DATA static uint32_t s_inbox_head;
CCM_DATA static uint32_t s_inbox_count;
CCM_DATA static courier_t s_couriers[ARDA_COURIERS];

CCM_DATA static arda_stats_t s_stats; /* K6: alanlarin yazari arda.h'de */

static TaskHandle_t s_task;
CCM_DATA static StackType_t s_stack[STACK_WORDS_ARDA_FRAME];
CCM_DATA static StaticTask_t s_tcb;

static const qexp_message_t *msg_of(port_handle_t h)
{
    return (const qexp_message_t *)port_slot(PORT_ARDA_MSG, h);
}

/* n mesajlik seferin suresi: hat n mesaji arka arkaya gonderir, bitis bir
 * sonraki cerceve sinirinda gorulur. */
static uint32_t trip_frames_for(uint32_t n)
{
    return (n * s_msg_us + ARDA_FRAME_US - 1U) / ARDA_FRAME_US;
}

static void complete_trips(uint32_t frame_no)
{
    courier_t *done_c[ARDA_COURIERS];
    uint32_t nd = 0U;
    uint32_t kmax = 0U;
    for (uint32_t i = 0U; i < ARDA_COURIERS; i++)
    {
        courier_t *c = &s_couriers[i];
        if (c->n != 0U && (frame_no - c->start_frame) >= c->trip_frames)
        {
            done_c[nd++] = c;
            kmax = (c->n > kmax) ? c->n : kmax;
        }
    }
    if (nd == 0U)
    {
        return;
    }
    /* Konum sirasiyla (once her hattin 0. mesaji, sonra 1. ...): seritlenmis
     * yuk sira numarasiyla teslim edilir. */
    const uint32_t done_us = timestamp_now_us();
    for (uint32_t k = 0U; k < kmax; k++)
    {
        for (uint32_t j = 0U; j < nd; j++)
        {
            const courier_t *c = done_c[j];
            if (k >= c->n)
            {
                continue;
            }
            const port_handle_t h = c->load[k];
            const qexp_message_t *m = msg_of(h);
            APP_TRACE_Q_DONE(m->sequence);
            qexp_consumer_done(m, s_valid[h], c->start_us + k * s_msg_us, done_us);
            port_release(PORT_ARDA_MSG, h); /* tam bir kez */
        }
    }
    for (uint32_t j = 0U; j < nd; j++)
    {
        done_c[j]->n = 0U;
    }
}

static void intake(void)
{
    for (;;)
    {
        port_handle_t h;
        const port_status_t st = port_take(PORT_ARDA_MSG, &h);
        if (st == PORT_EMPTY)
        {
            break;
        }
        if (st != PORT_OK)
        {
            /* Gecersiz tutamac: port bildirdi, slota erisilmez. Mesaj
             * kimligi bilinmedigi icin yalnizca tuketilmis sayilir ki kosu
             * bitebilsin (kaydi bos kalir, kontroller basarisiz olur). */
            qexp_consumer_drop();
            continue;
        }
        /* K5: gelen kutusu port kapasitesi kadar; alinan her tutamac sigar. */
        configASSERT(s_inbox_count < ARDA_POOL_LEN);

        const qexp_message_t *m = msg_of(h);
        const uint32_t age_us = timestamp_now_us() - m->generated_us;
        APP_TRACE_Q_RECEIVE(m->sequence, age_us);
        if (age_us > s_stats.intake_max_us)
        {
            s_stats.intake_max_us = age_us;
        }
        s_valid[h] = qexp_consumer_take(m);

        s_inbox[(s_inbox_head + s_inbox_count) % ARDA_POOL_LEN] = h;
        s_inbox_count++;
        if (s_inbox_count > s_stats.inbox_max)
        {
            s_stats.inbox_max = s_inbox_count;
        }
    }
}

static void dispatch(uint32_t frame_no, uint32_t frame_us)
{
    courier_t *free_c[ARDA_COURIERS];
    uint32_t nf = 0U;
    for (uint32_t i = 0U; i < ARDA_COURIERS; i++)
    {
        if (s_couriers[i].n == 0U)
        {
            free_c[nf++] = &s_couriers[i];
        }
    }

    /* Seritleme: k. turda her bos kuryeye birer mesaj. Ayni anda yola cikan
     * hatlarda k. konumdakiler ayni anda baslar; servis baslari sira
     * numarasini izler. Bekleyen yoksa yuk dolmadan yola cikilir. */
    for (uint32_t k = 0U; k < ARDA_BATCH_MAX && s_inbox_count > 0U; k++)
    {
        for (uint32_t j = 0U; j < nf && s_inbox_count > 0U; j++)
        {
            courier_t *c = free_c[j];
            const port_handle_t h = s_inbox[s_inbox_head];
            s_inbox_head = (s_inbox_head + 1U) % ARDA_POOL_LEN;
            s_inbox_count--;
            c->load[c->n++] = h;

            /* K7: gec kalan mesaj atilmaz, servis edilir ve bildirilir. k.
             * mesajin servisi sefer basindan k mesaj sonra baslar. */
            if ((frame_us + k * s_msg_us - msg_of(h)->generated_us) > ARDA_DEADLINE_US)
            {
                s_stats.deadline_miss++;
                hm_report(HM_DEADLINE_MISS);
            }
        }
    }
    for (uint32_t j = 0U; j < nf; j++)
    {
        courier_t *c = free_c[j];
        if (c->n == 0U)
        {
            continue;
        }
        c->start_frame = frame_no;
        c->start_us = frame_us;
        c->trip_frames = trip_frames_for(c->n);
        if (c->n > s_stats.batch_max)
        {
            s_stats.batch_max = c->n;
        }
    }

    uint32_t busy = 0U;
    for (uint32_t i = 0U; i < ARDA_COURIERS; i++)
    {
        busy += (s_couriers[i].n != 0U) ? 1U : 0U;
    }
    if (busy > s_stats.courier_busy_max)
    {
        s_stats.courier_busy_max = busy;
    }
}

static void write_status(void)
{
    arda_status_t st = {
        .frame = s_stats.frames,
        .backlog = s_inbox_count,
        .oldest_wait_us = 0U,
        .deadline_miss = s_stats.deadline_miss,
    };
    if (s_inbox_count > 0U)
    {
        st.oldest_wait_us = timestamp_now_us() - msg_of(s_inbox[s_inbox_head])->generated_us;
    }
    (void)port_write(PORT_ARDA_STATUS, &st);
}

static bool all_idle(void)
{
    if (s_inbox_count != 0U)
    {
        return false;
    }
    for (uint32_t i = 0U; i < ARDA_COURIERS; i++)
    {
        if (s_couriers[i].n != 0U)
        {
            return false;
        }
    }
    return true;
}

static void frame_stats(uint32_t t_us, uint32_t *prev_us)
{
    if (s_stats.frames > 0U)
    {
        const uint32_t dt = t_us - *prev_us;
        if (dt < s_stats.frame_dt_min_us)
        {
            s_stats.frame_dt_min_us = dt;
        }
        if (dt > s_stats.frame_dt_max_us)
        {
            s_stats.frame_dt_max_us = dt;
        }
    }
    *prev_us = t_us;
    s_stats.frames++;
}

static void arda_frame_task(void *arg)
{
    (void)arg;

    for (;;)
    {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY); /* bildirim indeksi 0: kosu basi */

        frame_timer_start(s_task);
        uint32_t frame_no = 0U;
        uint32_t prev_us = 0U;
        for (;;)
        {
            /* Cerceve sinirina kadar bekle. n > 1: cerceve isi bitmeden
             * sonraki sinir(lar) gecmisti; gecen cerceveler kacirilmis sayilir,
             * seferler yine gercek cerceve sayisiyla ilerler. */
            const uint32_t n = frame_timer_wait();
            s_stats.frame_miss += n - 1U;
            frame_no += n;
            const uint32_t t0 = timestamp_now_us();
            const uint32_t phase = frame_timer_since_tick_us();
            if (phase < s_stats.frame_phase_min_us)
            {
                s_stats.frame_phase_min_us = phase;
            }
            if (phase > s_stats.frame_phase_max_us)
            {
                s_stats.frame_phase_max_us = phase;
            }
            frame_stats(t0, &prev_us);

            complete_trips(frame_no);
            intake();
            dispatch(frame_no, t0);
            write_status();

            const uint32_t work_us = timestamp_now_us() - t0;
            if (work_us > s_stats.frame_work_max_us)
            {
                s_stats.frame_work_max_us = work_us;
            }
            if (qexp_consumer_finished() && all_idle())
            {
                break;
            }
        }
        frame_timer_stop();
        if (s_stats.frames < 2U)
        {
            s_stats.frame_dt_min_us = 0U;
        }
        if (s_stats.frames == 0U)
        {
            s_stats.frame_phase_min_us = 0U;
        }
        /* K6: Producer bitti, her mesaj servis edildi; butun tutamaclar
         * portun bos kuyrugunda olmali. */
        s_stats.pool_leak = ARDA_POOL_LEN - port_free_count(PORT_ARDA_MSG);
        qexp_finish_run();
    }
}

/* ---- Yasam dongusu ---- */

void arda_init(void)
{
    s_task = xTaskCreateStatic(arda_frame_task, "Arda", STACK_WORDS_ARDA_FRAME, NULL, PRIO_ARDA_FRAME,
                               s_stack, &s_tcb);
    configASSERT(s_task != NULL);
    frame_timer_init();
}

uint16_t arda_service_ms(arda_service_t service)
{
    const TickType_t t = (service == ARDA_SERVICE_TX) ? ARDA_TRIP_TICKS_TX : ARDA_TRIP_TICKS_FIXED;
    return (uint16_t)((t * ARDA_TICK_US) / 1000U);
}

void arda_run_begin(arda_service_t service)
{
    s_msg_us = (service == ARDA_SERVICE_TX) ? ARDA_TX_US : (ARDA_TRIP_MS * 1000U);

    /* Kosular arasi hicbir gorev tutamac tutmaz; portlar bastan kurulur. */
    port_reset(PORT_ARDA_MSG);
    port_reset(PORT_ARDA_STATUS);
    hm_reset();
    memset(s_valid, 0, sizeof(s_valid));
    memset(s_couriers, 0, sizeof(s_couriers));
    s_inbox_head = 0U;
    s_inbox_count = 0U;

    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.frame_dt_min_us = UINT32_MAX;
    s_stats.frame_phase_min_us = UINT32_MAX;
}

TaskHandle_t arda_task(void)
{
    return s_task;
}

void arda_get_stats(arda_stats_t *out)
{
    *out = s_stats;
}

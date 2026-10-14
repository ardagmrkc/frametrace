#include "queue_exp.h"
#include "queue_exp_core.h"
#include "arda.h"
#include "port_layer.h"
#include "main.h"
#include "protocol.h"
#include "timestamp.h"
#include "tx_queue.h"
#include "app_trace.h"
#include "mem_layout.h"
#include "task.h"
#include "queue.h"
#include <string.h>

/*
 * Iki statik kuyruk: A ve C kucugu (4), B buyugu (16) kullanir; Arda port
 * katmanini (port_layer.h: PORT_ARDA_MSG, 32 slot) kullanir. Kapasite kosu
 * basinda kuyruk secilerek degisir; kuyruk hic silinip yeniden olusturulmaz. Kosu sinirinda kuyruklar bostur
 * ve uzerlerinde bekleyen gorev yoktur: tuketici yalnizca kosu suresince
 * alir, kosu bitince bildirim bekler.
 *
 * Tuketici surume gore secilir (s_run_consumer): A/B/C'de consumer_task,
 * Arda'da cerceve gorevi (arda.c). Ikisi de kayitlari queue_exp_core.h
 * fonksiyonlariyla yazar.
 *
 * Kosu yasami (s_state):
 *   IDLE    -> RUNNING  queue_exp_start (TelemetryTask, komutla)
 *   RUNNING -> DUMPING  Consumer: Producer bitti ve kabul edilen her mesajin
 *                       servisi tamamlandi; UartTxTask'a doku istegi gider
 *   DUMPING -> IDLE     UartTxTask kayitlari gonderdi (queue_exp_dump_done)
 *
 * Kayit sahipligi: s_prod[] ve Producer sayaclarini yalnizca Producer,
 * s_cons[] ve Consumer sayaclarini yalnizca Consumer yazar. UartTxTask
 * hepsini ancak DUMPING'de okur; queue_exp_start ancak IDLE'da sifirlar.
 */

typedef enum
{
    QEXP_IDLE = 0,
    QEXP_RUNNING,
    QEXP_DUMPING,
} qexp_state_t;

typedef struct
{
    uint32_t gen_us;  /* TIM2: generated_us */
    uint32_t send_us; /* send_call_us */
    uint8_t depth;
    bool sent_ok;
} prod_record_t;

typedef struct
{
    uint32_t recv_us;
    uint32_t done_us;
    bool received;
} cons_record_t;

/* Surum tablosu (queue_exp.h). A/B/C'de degisen yalnizca kapasite ve
 * bekleme; Arda (ek deney) ayrica tasimayi (tablo indeksi) ve tuketiciyi
 * degistirir (arda.h). Arda'da bekleme 0'dir: tablo dolmayacak sekilde
 * boyutlanir, dolarsa bu bir tasarim hatasidir ve gonderim hatasi sayilir. */
static const struct
{
    char name;
    uint8_t capacity;
    uint16_t send_wait_ms;
} k_variants[5] = {
    { 'A', QEXP_CAPACITY_SMALL, 0U },
    { 'B', QEXP_CAPACITY_LARGE, 0U },
    { 'C', QEXP_CAPACITY_SMALL, QEXP_SEND_WAIT_MS_C },
    /* { 'D', QEXP_CAPACITY_ARDA, 0U },  Arda, adim 1 tek basina (servis 20 ms): kapali, bkz. arda.h */
    { '\0', 0U, 0U },               /* 3: kullanilmiyor (command.c reddeder) */
    { 'E', QEXP_CAPACITY_ARDA, 0U }, /* Arda2: adim 1 + 2 + 3 */
};

static QueueHandle_t s_q_small;
static QueueHandle_t s_q_large;
static StaticQueue_t s_q_small_ctrl;
static StaticQueue_t s_q_large_ctrl;
static uint8_t s_q_small_items[QEXP_CAPACITY_SMALL * sizeof(qexp_message_t)];
static uint8_t s_q_large_items[QEXP_CAPACITY_LARGE * sizeof(qexp_message_t)];

static TaskHandle_t s_producer;
static TaskHandle_t s_consumer;
static TaskHandle_t s_run_consumer; /* bu kosunun tuketicisi (queue_exp_start secer) */
CCM_DATA static StackType_t s_producer_stack[STACK_WORDS_QEXP_PRODUCER]; /* mem_layout.h */
CCM_DATA static StackType_t s_consumer_stack[STACK_WORDS_QEXP_CONSUMER];
CCM_DATA static StaticTask_t s_producer_tcb;
CCM_DATA static StaticTask_t s_consumer_tcb;

static volatile qexp_state_t s_state = QEXP_IDLE;
static uint16_t s_run_no;

/* Kosu tanimi: queue_exp_start yazar (IDLE iken), gorevler bildirimden
 * sonra okur. */
static qexp_run_info_t s_info;
static QueueHandle_t s_queue;
static TickType_t s_send_wait;

static prod_record_t s_prod[QEXP_ATTEMPTS];
static cons_record_t s_cons[QEXP_ATTEMPTS];

/* Producer yazar. s_producer_done en son yazilir: true gorulduyse
 * s_accepted artik degismez. */
static volatile uint32_t s_attempted;
static volatile uint32_t s_accepted;
static volatile uint32_t s_failed;
static volatile uint32_t s_queue_max;
static volatile bool s_producer_done;
static uint32_t s_run_start_us;
static uint32_t s_status_backlog_max; /* Arda: durum portunda gorulen en buyuk birikme */

/* Kosunun tuketicisi yazar (queue_exp_core.h). */
static volatile uint32_t s_consumed;
static uint32_t s_rx_idle_timeout;
static uint32_t s_order_error;
static uint32_t s_last_done_us;
static uint32_t s_last_seq;
static uint32_t s_dump_retry; /* doku istegi UART kuyrugunda yer bulamadi (0 olmali) */

/* Tek gonderme girisimi. generated_us ile send_call_us'in baslangici ayni
 * andir: SEND_BEGIN izi olcumden ONCE, SEND_OK/SEND_FAIL SONRA yazilir;
 * izin kendi maliyeti send_call_us'e girmez. */
/* Arda: tuketicinin son durumu (sampling port). Bayatsa port saglik
 * izleyiciye bildirir. Gonderme karari degismez: olay verisi atilmaz ve yuk
 * tarifi sabittir; deger yalnizca izlenir. */
static void read_status(void)
{
    arda_status_t st;
    port_sample_info_t info;
    if (port_read(PORT_ARDA_STATUS, &st, &info) != PORT_EMPTY && st.backlog > s_status_backlog_max)
    {
        s_status_backlog_max = st.backlog;
    }
}

static void send_one(uint32_t seq)
{
    prod_record_t *r = &s_prod[seq - 1U];
    qexp_message_t msg = { .sequence = seq, .value = s_info.run };
    const bool arda = qexp_variant_is_arda(s_info.variant);

    if (arda)
    {
        read_status(); /* olcumden once: send_call'a girmez */
    }
    APP_TRACE_Q_SEND_BEGIN(seq);
    const uint32_t t_begin = timestamp_now_us();
    msg.generated_us = t_begin;
    /* Arda: mesaj portun slotuna, tutamaci portun kuyruguna (port_layer.h).
     * A/B/C: mesaj kuyruga. */
    const BaseType_t res = arda ? ((port_send(PORT_ARDA_MSG, &msg) == PORT_OK) ? pdPASS : errQUEUE_FULL)
                                : xQueueSend(s_queue, &msg, s_send_wait);
    const uint32_t t_end = timestamp_now_us();

    if (seq == 1U)
    {
        s_run_start_us = t_begin;
    }
    r->gen_us = t_begin;
    r->send_us = t_end - t_begin;
    s_attempted++;

    if (res == pdPASS)
    {
        /* A/B/C: Producer tuketiciden yuksek oncelikli; gonderimden sonra
         * tuketici araya giremez, okunan doluluk bu gonderimin hemen
         * sonrasidir. Arda: cerceve gorevi (P5) arada bir tick'e denk
         * gelirse kuyrugu bosaltmis olabilir; doluluk o zaman dusuk okunur. */
        const UBaseType_t depth = uxQueueMessagesWaiting(s_queue);
        r->sent_ok = true;
        r->depth = (uint8_t)depth;
        s_accepted++;
        if (depth > s_queue_max)
        {
            s_queue_max = depth;
        }
        APP_TRACE_Q_SEND_OK(seq, r->send_us, depth);
    }
    else
    {
        /* Kuyruk dolu ve bekleme suresi doldu (A/B: hic beklenmedi). Mesaj
         * kaybolur; kaydi kalir, gecikmesi yoktur. */
        s_failed++;
        APP_TRACE_Q_SEND_FAIL(seq, r->send_us);
    }
}

static void producer_task(void *arg)
{
    (void)arg;

    for (;;)
    {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        xTaskNotifyGive(s_run_consumer);          /* tuketici kuyrugu dinlemeye baslar */
        vTaskDelay(pdMS_TO_TICKS(QEXP_SETTLE_MS)); /* QRUN satiri hattan ciksin */
        APP_TRACE_Q_RUN_BEGIN(s_info.run, s_info.variant, s_info.load == QEXP_LOAD_BURST, s_queue,
                              s_info.capacity);

        for (uint32_t seq = 1U; seq <= QEXP_ATTEMPTS; seq++)
        {
            send_one(seq);
            const uint32_t gap_ms = qexp_gap_after_ms(s_info.load, seq);
            if (gap_ms > 0U)
            {
                vTaskDelay(pdMS_TO_TICKS(gap_ms));
            }
        }
        s_producer_done = true;
    }
}

/* ---- Tuketici arayuzu (queue_exp_core.h) ---- */

bool qexp_consumer_take(const qexp_message_t *msg)
{
    const bool valid = (msg->value == s_info.run) && (msg->sequence > s_last_seq) &&
                       (msg->sequence <= QEXP_ATTEMPTS);
    if (valid)
    {
        s_last_seq = msg->sequence;
    }
    else
    {
        s_order_error++;
    }
    return valid;
}

void qexp_consumer_done(const qexp_message_t *msg, bool valid, uint32_t start_us, uint32_t done_us)
{
    if (valid)
    {
        cons_record_t *c = &s_cons[msg->sequence - 1U];
        c->recv_us = start_us;
        c->done_us = done_us;
        c->received = true;
    }
    s_last_done_us = done_us;
    s_consumed++;
}

void qexp_consumer_drop(void)
{
    s_consumed++;
}

bool qexp_consumer_finished(void)
{
    return s_producer_done && s_consumed == s_accepted;
}

/* Doku istegi icin UART kuyrugunun en gec bosalma suresi: kuyruk tam doluyken
 * UartTxTask her satiri (PROTOCOL_FRAME_SIZE bayt, bayt basina 10 bit)
 * UART_BAUDRATE'te gonderir. 16 satir x 2,78 ms = 44 ms (230400 baud). */
#define QEXP_UART_DRAIN_MS \
    ((UART_TX_QUEUE_LEN * PROTOCOL_FRAME_SIZE * 10U * 1000U + UART_BAUDRATE - 1U) / UART_BAUDRATE)
/* Yeniden deneme siniri bunun iki kati: o surede yer acilmadiysa UartTxTask
 * calismiyordur, bu bir tasarim hatasidir. */
#define QEXP_DUMP_RETRY_TICKS pdMS_TO_TICKS(2U * QEXP_UART_DRAIN_MS)

/* Kosu bitti: Producer'in son girisimi yapildi ve kabul edilen her mesajin
 * servisi tamamlandi (kuyruk bos). Doku UartTxTask'ta yapilir; hatti tek
 * sahibi odur.
 *
 * Doku istegi 0 beklemeyle konur. Kosu boyunca telemetri susar (S0); UART
 * kuyruguna yalnizca buton yolu yazabilir, o da beklemeden. Kuyruk doluysa
 * (olmamali) bir tick sonra yeniden denenir ve her deneme sayilir
 * (dump_retry); QEXP_DUMP_RETRY_TICKS icinde yer acilmazsa configASSERT. */
void qexp_finish_run(void)
{
    APP_TRACE_Q_RUN_END(s_info.run);

    taskENTER_CRITICAL();
    s_state = QEXP_DUMPING;
    taskEXIT_CRITICAL();

    const tx_message_t m = { .type = MSG_CMD_QDUMP };
    uint32_t tries = 0U;
    while (!tx_queue_send(&m, 0))
    {
        s_dump_retry++;
        tries++;
        configASSERT(tries < QEXP_DUMP_RETRY_TICKS);
        vTaskDelay(1);
    }
}

static void consumer_task(void *arg)
{
    (void)arg;

    for (;;)
    {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        for (;;)
        {
            qexp_message_t msg;
            if (xQueueReceive(s_queue, &msg, pdMS_TO_TICKS(QEXP_RX_TIMEOUT_MS)) == pdPASS)
            {
                const uint32_t recv_us = timestamp_now_us();
                APP_TRACE_Q_RECEIVE(msg.sequence, recv_us - msg.generated_us);
                const bool valid = qexp_consumer_take(&msg);

                /* Modellenen servis (ders-03): bloklu bekleme, CPU yuku degil.
                 * Servis alinir alinmaz basladigi icin g2r = alma anidir. */
                vTaskDelay(pdMS_TO_TICKS(QEXP_SERVICE_MS));
                const uint32_t done_us = timestamp_now_us();
                APP_TRACE_Q_DONE(msg.sequence);
                qexp_consumer_done(&msg, valid, recv_us, done_us);

                if (qexp_consumer_finished())
                {
                    break;
                }
            }
            else if (s_producer_done)
            {
                break; /* kuyruk bos ve Producer bitti */
            }
            else
            {
                s_rx_idle_timeout++; /* olmamali: iki girisim arasi QEXP_RX_TIMEOUT_MS'den kisa */
            }
        }

        qexp_finish_run();
    }
}

void queue_exp_init(void)
{
    s_q_small = xQueueCreateStatic(QEXP_CAPACITY_SMALL, sizeof(qexp_message_t), s_q_small_items,
                                   &s_q_small_ctrl);
    s_q_large = xQueueCreateStatic(QEXP_CAPACITY_LARGE, sizeof(qexp_message_t), s_q_large_items,
                                   &s_q_large_ctrl);
    configASSERT(s_q_small != NULL);
    configASSERT(s_q_large != NULL);

    s_consumer = xTaskCreateStatic(consumer_task, "Consumer", STACK_WORDS_QEXP_CONSUMER, NULL,
                                   PRIO_QEXP_CONSUMER, s_consumer_stack, &s_consumer_tcb);
    s_producer = xTaskCreateStatic(producer_task, "Producer", STACK_WORDS_QEXP_PRODUCER, NULL,
                                   PRIO_QEXP_PRODUCER, s_producer_stack, &s_producer_tcb);
    configASSERT(s_consumer != NULL);
    configASSERT(s_producer != NULL);
    s_run_consumer = s_consumer;

    /* Arda: portlar (port_config.c) ve cerceve gorevi; her portun tek yazari
     * ve tek okuyucusu burada baglanir (port_layer.h P3). */
    port_init();
    arda_init();
    port_bind(PORT_ARDA_MSG, s_producer, arda_task());
    port_bind(PORT_ARDA_STATUS, arda_task(), s_producer);
}

bool queue_exp_start(uint8_t cmd_arg, qexp_run_info_t *info)
{
    bool busy;
    taskENTER_CRITICAL();
    busy = (s_state != QEXP_IDLE);
    if (!busy)
    {
        s_state = QEXP_RUNNING;
    }
    taskEXIT_CRITICAL();

    if (busy)
    {
        *info = s_info; /* calisan (ya da dokulen) kosu */
        return false;
    }

    const uint8_t v = cmd_arg & CMD_QRUN_VARIANT_MASK;
    configASSERT(v < 5U && k_variants[v].name != '\0'); /* command.c gecersiz surumu zaten atar */

    if (++s_run_no == 0U)
    {
        s_run_no = 1U;
    }
    s_info.run = s_run_no;
    s_info.variant = k_variants[v].name;
    s_info.load = ((cmd_arg & CMD_QRUN_LOAD_BURST) != 0U) ? QEXP_LOAD_BURST : QEXP_LOAD_NORMAL;
    s_info.capacity = k_variants[v].capacity;
    s_send_wait = pdMS_TO_TICKS(k_variants[v].send_wait_ms);
    s_info.send_wait_ticks = (uint16_t)s_send_wait;
    if (qexp_variant_is_arda(s_info.variant))
    {
        /* const arda_service_t service = (s_info.variant == 'E') ? ARDA_SERVICE_TX : ARDA_SERVICE_FIXED; */
        const arda_service_t service = ARDA_SERVICE_TX; /* tek surum: Arda2 */
        arda_run_begin(service); /* portlar bos, sayaclar sifir, sefer suresi */
        s_info.service_ms = arda_service_ms(service);
        s_queue = port_ready_queue(PORT_ARDA_MSG);
        s_run_consumer = arda_task();
    }
    else
    {
        s_info.service_ms = QEXP_SERVICE_MS;
        s_queue = (s_info.capacity == QEXP_CAPACITY_LARGE) ? s_q_large : s_q_small;
        s_run_consumer = s_consumer;
    }

    memset(s_prod, 0, sizeof(s_prod));
    memset(s_cons, 0, sizeof(s_cons));
    s_attempted = 0U;
    s_accepted = 0U;
    s_failed = 0U;
    s_queue_max = 0U;
    s_run_start_us = 0U;
    s_status_backlog_max = 0U;
    s_consumed = 0U;
    s_rx_idle_timeout = 0U;
    s_order_error = 0U;
    s_last_done_us = 0U;
    s_last_seq = 0U;
    s_dump_retry = 0U;
    s_producer_done = false;

    *info = s_info;
    xTaskNotifyGive(s_producer);
    return true;
}

void queue_exp_get_info(qexp_run_info_t *out)
{
    *out = s_info;
}

bool queue_exp_get_record(uint32_t seq, qexp_record_t *out)
{
    if (seq == 0U || seq > QEXP_ATTEMPTS)
    {
        return false;
    }
    const prod_record_t *p = &s_prod[seq - 1U];
    const cons_record_t *c = &s_cons[seq - 1U];

    out->sent_ok = p->sent_ok;
    out->received = c->received;
    out->depth = p->depth;
    out->t_us = p->gen_us - s_run_start_us;
    out->send_us = p->send_us;
    out->g2r_us = c->received ? (c->recv_us - p->gen_us) : 0U;
    out->svc_us = c->received ? (c->done_us - c->recv_us) : 0U;
    return true;
}

void queue_exp_get_summary(qexp_summary_t *out)
{
    out->attempted = s_attempted;
    out->accepted = s_accepted;
    out->failed = s_failed;
    out->consumed = s_consumed;
    out->queue_max = s_queue_max;
    out->rx_idle_timeout = s_rx_idle_timeout;
    out->order_error = s_order_error;
    out->run_us = (s_consumed > 0U) ? (s_last_done_us - s_run_start_us) : 0U;
    out->stack_hwm_producer = uxTaskGetStackHighWaterMark(s_producer);
    out->stack_hwm_consumer = uxTaskGetStackHighWaterMark(s_run_consumer); /* Arda: cerceve gorevi */
    out->dump_retry = s_dump_retry;
    out->status_backlog_max = s_status_backlog_max;
}

void queue_exp_dump_done(void)
{
    taskENTER_CRITICAL();
    s_state = QEXP_IDLE;
    taskEXIT_CRITICAL();
}

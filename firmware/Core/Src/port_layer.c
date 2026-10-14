#include "port_layer.h"
#include "health.h"
#include "timestamp.h"
#include <string.h>

/*
 * Port katmani (port_layer.h, kurallar P1..P6).
 *
 * Queuing port: slotlar statik (port_config.c); bos tutamaclar free
 * kuyrugunda, gonderilmis ama alinmamislar ready kuyrugunda (FIFO). Iki
 * kuyruk da kapasite kadardir: tutamac alinabildiyse yerlestirme basarisiz
 * olamaz. Slotun icerigine yalnizca tutamacin o anki sahibi dokunur; bu
 * yuzden kopyalama kritik bolum gerektirmez.
 *
 * Sampling port: tek slot. Yazan ve okuyan ayni slota dokundugu icin kopya
 * kritik bolumde yapilir; boyut port_config.c'de sinirlidir (kisa ve sabit
 * sureli).
 */

/* P4: queuing port slot durumu. */
#define SLOT_FREE   0U /* free kuyrugunda */
#define SLOT_QUEUED 1U /* yazan gonderdi: ready kuyrugunda */
#define SLOT_TAKEN  2U /* okuyan aldi */

typedef struct
{
    QueueHandle_t free_q;  /* queuing */
    QueueHandle_t ready_q; /* queuing */
    TaskHandle_t writer;
    TaskHandle_t reader;
    port_stats_t stats;
    uint32_t written_us;   /* sampling: yazan, kritik bolumde */
    uint32_t read_writes;  /* sampling, okuyan: son okumada gorulen stats.writes (0: henuz okunmadi) */
} port_rt_t;

static port_rt_t s_rt[PORT_COUNT];

static const port_def_t *def_of(port_id_t p, port_kind_t kind)
{
    configASSERT((uint32_t)p < (uint32_t)PORT_COUNT);
    configASSERT(g_port_defs[p].kind == kind);
    return &g_port_defs[p];
}

/* P3: yalnizca bagli gorev. */
static void check_access(TaskHandle_t owner)
{
    if (owner == NULL || xTaskGetCurrentTaskHandle() != owner)
    {
        hm_report(HM_PORT_ACCESS);
    }
}

static uint8_t *slot_at(const port_def_t *d, port_handle_t h)
{
    return (uint8_t *)d->slots + ((uint32_t)h * d->msg_size);
}

/* ---- Yonetim ---- */

void port_init(void)
{
    for (uint32_t i = 0U; i < (uint32_t)PORT_COUNT; i++)
    {
        const port_def_t *d = &g_port_defs[i];
        port_rt_t *r = &s_rt[i];
        if (d->kind == PORT_QUEUING)
        {
            r->free_q = xQueueCreateStatic(d->capacity, sizeof(port_handle_t), d->free_items, d->free_ctrl);
            r->ready_q = xQueueCreateStatic(d->capacity, sizeof(port_handle_t), d->ready_items, d->ready_ctrl);
            configASSERT(r->free_q != NULL);
            configASSERT(r->ready_q != NULL);
        }
        port_reset((port_id_t)i);
    }
}

void port_bind(port_id_t p, TaskHandle_t writer, TaskHandle_t reader)
{
    configASSERT((uint32_t)p < (uint32_t)PORT_COUNT);
    configASSERT(writer != NULL && reader != NULL && writer != reader);
    s_rt[p].writer = writer;
    s_rt[p].reader = reader;
}

void port_reset(port_id_t p)
{
    configASSERT((uint32_t)p < (uint32_t)PORT_COUNT);
    const port_def_t *d = &g_port_defs[p];
    port_rt_t *r = &s_rt[p];

    if (d->kind == PORT_QUEUING)
    {
        /* Portu kimse kullanmiyor: hicbir gorev tutamac tutmaz. */
        (void)xQueueReset(r->free_q);
        (void)xQueueReset(r->ready_q);
        for (uint32_t i = 0U; i < d->capacity; i++)
        {
            const port_handle_t h = (port_handle_t)i;
            d->state[i] = SLOT_FREE;
            (void)xQueueSend(r->free_q, &h, 0);
        }
    }
    memset(d->slots, 0, (size_t)d->capacity * d->msg_size);
    memset(&r->stats, 0, sizeof(r->stats));
    r->written_us = 0U;
    r->read_writes = 0U;
}

/* ---- Queuing port ---- */

port_status_t port_send(port_id_t p, const void *msg)
{
    const port_def_t *d = def_of(p, PORT_QUEUING);
    port_rt_t *r = &s_rt[p];
    check_access(r->writer);

    port_handle_t h = PORT_HANDLE_NONE;
    if (xQueueReceive(r->free_q, &h, 0) != pdPASS)
    {
        r->stats.overflow++; /* tek kapasite siniri */
        hm_report(HM_PORT_OVERFLOW);
        return PORT_FULL;
    }
    if (h >= d->capacity || d->state[h] != SLOT_FREE)
    {
        hm_report(HM_PORT_ALLOC_ERROR); /* P4: bu tutamaca dokunulmaz */
        return PORT_ERROR;
    }
    memcpy(slot_at(d, h), msg, d->msg_size);
    d->state[h] = SLOT_QUEUED;
    const BaseType_t sent = xQueueSend(r->ready_q, &h, 0); /* tutamac alindiysa yer vardir */
    configASSERT(sent == pdPASS);
    (void)sent;
    r->stats.sent++;
    return PORT_OK;
}

port_status_t port_take(port_id_t p, port_handle_t *h)
{
    const port_def_t *d = def_of(p, PORT_QUEUING);
    port_rt_t *r = &s_rt[p];
    check_access(r->reader);

    port_handle_t got = PORT_HANDLE_NONE;
    *h = PORT_HANDLE_NONE;
    if (xQueueReceive(r->ready_q, &got, 0) != pdPASS)
    {
        return PORT_EMPTY;
    }
    if (got >= d->capacity || d->state[got] != SLOT_QUEUED)
    {
        hm_report(HM_PORT_FREE_ERROR); /* P4: slota erisilmez */
        return PORT_ERROR;
    }
    d->state[got] = SLOT_TAKEN;
    r->stats.received++;
    *h = got;
    return PORT_OK;
}

const void *port_slot(port_id_t p, port_handle_t h)
{
    const port_def_t *d = def_of(p, PORT_QUEUING);
    check_access(s_rt[p].reader);
    /* Okuyan yalnizca port_take'ten aldigi ve henuz geri vermedigi
     * tutamaci kullanir; aksi programlama hatasidir. */
    configASSERT(h < d->capacity && d->state[h] == SLOT_TAKEN);
    return slot_at(d, h);
}

void port_release(port_id_t p, port_handle_t h)
{
    const port_def_t *d = def_of(p, PORT_QUEUING);
    port_rt_t *r = &s_rt[p];
    check_access(r->reader);

    if (h >= d->capacity || d->state[h] != SLOT_TAKEN)
    {
        hm_report(HM_PORT_FREE_ERROR); /* P4: ornegin iki kez geri verme */
        return;
    }
    d->state[h] = SLOT_FREE;
    const BaseType_t back = xQueueSend(r->free_q, &h, 0); /* free kuyrugu kapasite kadar */
    configASSERT(back == pdPASS);
    (void)back;
}

uint32_t port_free_count(port_id_t p)
{
    (void)def_of(p, PORT_QUEUING);
    return (uint32_t)uxQueueMessagesWaiting(s_rt[p].free_q);
}

QueueHandle_t port_ready_queue(port_id_t p)
{
    (void)def_of(p, PORT_QUEUING);
    return s_rt[p].ready_q;
}

/* ---- Sampling port ---- */

port_status_t port_write(port_id_t p, const void *msg)
{
    const port_def_t *d = def_of(p, PORT_SAMPLING);
    port_rt_t *r = &s_rt[p];
    check_access(r->writer);

    taskENTER_CRITICAL();
    memcpy(d->slots, msg, d->msg_size);
    r->written_us = timestamp_now_us();
    r->stats.writes++;
    taskEXIT_CRITICAL();
    return PORT_OK;
}

port_status_t port_read(port_id_t p, void *msg, port_sample_info_t *info)
{
    const port_def_t *d = def_of(p, PORT_SAMPLING);
    port_rt_t *r = &s_rt[p];
    check_access(r->reader);

    info->age_us = 0U;
    info->missed = 0U;
    info->valid = false;

    uint32_t writes;
    uint32_t written_us;
    taskENTER_CRITICAL();
    writes = r->stats.writes;
    written_us = r->written_us;
    if (writes != 0U)
    {
        memcpy(msg, d->slots, d->msg_size);
    }
    taskEXIT_CRITICAL();

    if (writes == 0U)
    {
        return PORT_EMPTY; /* hic yazilmadi; msg degismedi */
    }

    info->age_us = timestamp_now_us() - written_us;
    /* Ilk okuma taban cizgisidir; sonrakilerde arada okunmadan ustune
     * yazilanlar sayilir. Ayni deger yeniden okunduysa atlanan yoktur. */
    if (r->read_writes != 0U && (writes - r->read_writes) > 1U)
    {
        info->missed = writes - r->read_writes - 1U;
    }
    r->read_writes = writes;
    info->valid = (info->age_us <= d->refresh_us);

    r->stats.reads++;
    r->stats.missed_updates += info->missed;
    if (info->age_us > r->stats.age_max_us)
    {
        r->stats.age_max_us = info->age_us;
    }
    if (!info->valid)
    {
        r->stats.stale_reads++;
        hm_report(HM_STALE_READ);
        return PORT_STALE; /* deger yine kopyalandi; kullanip kullanmamak okuyanin karari */
    }
    return PORT_OK;
}

void port_get_stats(port_id_t p, port_stats_t *out)
{
    configASSERT((uint32_t)p < (uint32_t)PORT_COUNT);
    *out = s_rt[p].stats;
}

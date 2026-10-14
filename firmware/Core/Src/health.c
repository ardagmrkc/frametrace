#include "health.h"
#include "FreeRTOS.h"
#include "task.h"

typedef enum
{
    HM_LOG = 0,
    HM_HALT,
} hm_action_t;

/* Olay basina tepki. Tasarim hatasi (yanlis gorevin porta erismesi) durur;
 * digerleri sayilir ve kosu sonunda raporlanir. Deadline kacirma mesaji
 * atmaz: olay verisi kaybolamaz (gereksinim KAY-04). */
static const hm_action_t k_action[HM_EVENT_COUNT] = {
    [HM_PORT_OVERFLOW] = HM_LOG,
    [HM_PORT_ALLOC_ERROR] = HM_LOG,
    [HM_PORT_FREE_ERROR] = HM_LOG,
    [HM_PORT_ACCESS] = HM_HALT,
    [HM_STALE_READ] = HM_LOG,
    [HM_DEADLINE_MISS] = HM_LOG,
};

static volatile uint32_t s_count[HM_EVENT_COUNT];

void hm_report(hm_event_t e)
{
    configASSERT(e < HM_EVENT_COUNT);
    taskENTER_CRITICAL();
    s_count[e]++;
    taskEXIT_CRITICAL();
    if (k_action[e] == HM_HALT)
    {
        configASSERT(0); /* tasarim hatasi */
    }
}

uint32_t hm_count(hm_event_t e)
{
    configASSERT(e < HM_EVENT_COUNT);
    return s_count[e];
}

void hm_reset(void)
{
    for (uint32_t i = 0U; i < (uint32_t)HM_EVENT_COUNT; i++)
    {
        s_count[i] = 0U;
    }
}

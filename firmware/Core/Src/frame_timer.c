#include "frame_timer.h"
#include "arda.h"
#include "main.h"
#include "timestamp.h"

/* Ilk karsilastirma en az bu kadar ileride olmali; yoksa bir periyot sonraya. */
#define FRAME_TIMER_MIN_LEAD_US 20U
#define SYSTICK_CYCLES_PER_US   (configCPU_CLOCK_HZ / 1000000U)

_Static_assert(ARDA_FRAME_PHASE_US >= FRAME_TIMER_MIN_LEAD_US && ARDA_FRAME_PHASE_US < ARDA_FRAME_US,
               "faz cercevenin icinde olmali");
_Static_assert(configTASK_NOTIFICATION_ARRAY_ENTRIES > FRAME_TIMER_NOTIFY_INDEX,
               "cerceve bildirimi icin ikinci bildirim indeksi gerekir");

static TaskHandle_t s_task;

uint32_t frame_timer_since_tick_us(void)
{
    /* SysTick asagi sayar; sifirdan LOAD'a donerken tick kesmesi gelir. */
    return (SysTick->LOAD - SysTick->VAL) / SYSTICK_CYCLES_PER_US;
}

uint32_t frame_timer_phase_us(void)
{
    return ARDA_FRAME_PHASE_US;
}

void frame_timer_init(void)
{
    TIM2->DIER &= ~TIM_DIER_CC1IE;
    NVIC_SetPriority(TIM2_IRQn, configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY);
    NVIC_EnableIRQ(TIM2_IRQn);
}

void frame_timer_start(TaskHandle_t task)
{
    s_task = task;
    (void)ulTaskNotifyValueClearIndexed(task, FRAME_TIMER_NOTIFY_INDEX, UINT32_MAX);

    /* Iki sayac ayni anda okunur: son tick kenari = simdi - kenardan beri. */
    taskENTER_CRITICAL();
    const uint32_t now = TIM2->CNT;
    const uint32_t edge = now - frame_timer_since_tick_us();
    uint32_t first = edge + ARDA_FRAME_PHASE_US;
    if ((int32_t)(first - now) < (int32_t)FRAME_TIMER_MIN_LEAD_US)
    {
        first += ARDA_FRAME_US;
    }
    TIM2->CCR1 = first;              /* OC1 dondurulmus kip (sifirlama degeri): yalnizca bayrak */
    TIM2->SR = ~TIM_SR_CC1IF;        /* rc_w0: 0 yazilan bayrak silinir */
    TIM2->DIER |= TIM_DIER_CC1IE;
    taskEXIT_CRITICAL();
}

uint32_t frame_timer_wait(void)
{
    /* Iki periyotta cerceve gelmediyse saat durmustur: tasarim hatasi. */
    const uint32_t n = ulTaskNotifyTakeIndexed(FRAME_TIMER_NOTIFY_INDEX, pdTRUE, pdMS_TO_TICKS(2U * ARDA_FRAME_US / 1000U + 1U));
    configASSERT(n != 0U);
    return n;
}

void frame_timer_stop(void)
{
    taskENTER_CRITICAL();
    TIM2->DIER &= ~TIM_DIER_CC1IE;
    TIM2->SR = ~TIM_SR_CC1IF;
    taskEXIT_CRITICAL();
    if (s_task != NULL)
    {
        (void)ulTaskNotifyValueClearIndexed(s_task, FRAME_TIMER_NOTIFY_INDEX, UINT32_MAX);
    }
}

void frame_timer_isr(void)
{
    traceISR_ENTER();
    BaseType_t wake = pdFALSE;

    if ((TIM2->SR & TIM_SR_CC1IF) != 0U && (TIM2->DIER & TIM_DIER_CC1IE) != 0U)
    {
        TIM2->SR = ~TIM_SR_CC1IF;
        /* Sonraki cerceve. Kesme bir periyoda yakin geciktiyse karsilastirma
         * gecmiste kalir ve sayac donene kadar (71 dk) gelmezdi: gecen her
         * cerceve icin ayrica bildirim verilir, cerceve gorevi bunlari
         * kacirilmis sayar. */
        uint32_t next = TIM2->CCR1 + ARDA_FRAME_US;
        vTaskNotifyGiveIndexedFromISR(s_task, FRAME_TIMER_NOTIFY_INDEX, &wake);
        while ((int32_t)(next - TIM2->CNT) < (int32_t)FRAME_TIMER_MIN_LEAD_US)
        {
            next += ARDA_FRAME_US;
            vTaskNotifyGiveIndexedFromISR(s_task, FRAME_TIMER_NOTIFY_INDEX, &wake);
        }
        TIM2->CCR1 = next;
    }

    /* Tek cikis: kesme cikisi SystemView'e de buradan bildirilir. */
    portYIELD_FROM_ISR(wake);
}

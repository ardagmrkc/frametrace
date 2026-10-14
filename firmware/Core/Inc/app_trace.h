#ifndef APP_TRACE_H
#define APP_TRACE_H

#include <stdint.h>
#include <stdbool.h>
#include "FreeRTOS.h"

/*
 * SEGGER SystemView uygulama olaylari (odev-systemview, "Uygulama
 * isaretleri"). Ayni basisi uctan uca izlemek icin her olay event_id tasir.
 * Olaylar olcum noktasinin hemen SONRASINA konur: t0..t4 zamanlari
 * kaymaz, izlemenin maliyeti bir sonraki araliga duser.
 *
 *   SystemView'de    odev etiketi   nokta  yer
 *   BTN ACCEPT #id   BTN_ACCEPT     t0     EXTI0 ISR, kabul edilen basis (Mark baslar)
 *   BTN TASK #id     BTN_TASK       t1     ButtonTask olayi aldi
 *   BTN READY #id    BTN_READY      t2     BTN mesaji hazir, TX kuyruguna verilecek
 *   BTN ENQUEUE #id  BTN_ENQUEUE           TX kuyruguna girdi; q = bekleyen mesaj
 *   BTN DROP #id     BTN_DROP              dustu; at 1 = buton kuyrugu, 2 = TX kuyrugu
 *   BTN TX_START #id BTN_TX_START   t3     UART aktarimi basliyor
 *   BTN TX_TC #id    BTN_TX_TC      t4     TC kesmesi, son bit hattan cikti (Mark biter)
 *   "Pencere A/S5"                         yeni olcum penceresi (surum/senaryo)
 *
 * Hafta 3 queue deneyi (queue_exp.c), "W3" modulu. n = mesajin seq'i
 * (1..44); her mesaj SEND_BEGIN'den DONE'a (ya da SEND_FAIL'e) kadar
 * 1000 + n numarali bir Mark araligidir.
 *   W3 SEND_BEGIN  xQueueSend oncesi (Producer)
 *   W3 SEND_OK     xQueueSend pdPASS; us = send_call_us, q = sonraki doluluk
 *   W3 SEND_FAIL   xQueueSend basarisiz; us = send_call_us
 *   W3 RECEIVE     Consumer mesaji aldi; lat = generation_to_receive_us
 *   W3 DONE        modellenen 20 ms servis bitti
 *   "Kosu 3 C/yogun MsgQ4" / "Kosu 3 bitti"  kosunun basi ve sonu; kuyruk
 *                                         SystemView'de MsgQ4 / MsgQ16 adini alir
 *
 * USE_SYSVIEW (FreeRTOSConfig.h) 0 iken hepsi bos makrodur: izleme
 * kapali/acik olcumu ayni kaynaktan derlenir.
 */
#ifndef USE_SYSVIEW
#define USE_SYSVIEW 0
#endif

#if USE_SYSVIEW

#include "SEGGER_SYSVIEW.h"

enum
{
    APP_EV_ACCEPT = 0,
    APP_EV_TASK,
    APP_EV_READY,
    APP_EV_ENQUEUE,
    APP_EV_DROP,
    APP_EV_TX_START,
    APP_EV_TX_TC,
    APP_EV_COUNT
};

#define APP_DROP_AT_BUTTON_QUEUE 1U
#define APP_DROP_AT_TX_QUEUE     2U

extern SEGGER_SYSVIEW_MODULE g_app_trace_module;

/* SEGGER_SYSVIEW_Conf() icinde, SEGGER_SYSVIEW_Init() sonrasi cagrilir. */
void app_trace_register(void);

/* Yeni olcum penceresi: SystemView'e "Pencere <surum>/S<n>" yazar. */
void app_trace_window(char variant, uint8_t scenario);

#define APP_EV_ID(e) (g_app_trace_module.EventOffset + (unsigned)(e))

#define APP_TRACE_BTN_ACCEPT(id, sc)                                                      \
    do                                                                                    \
    {                                                                                     \
        SEGGER_SYSVIEW_RecordU32x2(APP_EV_ID(APP_EV_ACCEPT), (U32)(id), (U32)(sc));       \
        SEGGER_SYSVIEW_MarkStart((unsigned)(id));                                         \
    } while (0)
#define APP_TRACE_BTN_TASK(id)     SEGGER_SYSVIEW_RecordU32(APP_EV_ID(APP_EV_TASK), (U32)(id))
#define APP_TRACE_BTN_READY(id)    SEGGER_SYSVIEW_RecordU32(APP_EV_ID(APP_EV_READY), (U32)(id))
#define APP_TRACE_BTN_ENQUEUE(id, q) \
    SEGGER_SYSVIEW_RecordU32x2(APP_EV_ID(APP_EV_ENQUEUE), (U32)(id), (U32)(q))
#define APP_TRACE_BTN_DROP(id, at)                                                        \
    do                                                                                    \
    {                                                                                     \
        SEGGER_SYSVIEW_RecordU32x2(APP_EV_ID(APP_EV_DROP), (U32)(id), (U32)(at));         \
        SEGGER_SYSVIEW_MarkStop((unsigned)(id));                                          \
    } while (0)
#define APP_TRACE_BTN_TX_START(id) SEGGER_SYSVIEW_RecordU32(APP_EV_ID(APP_EV_TX_START), (U32)(id))
#define APP_TRACE_BTN_TX_TC(id)                                                           \
    do                                                                                    \
    {                                                                                     \
        SEGGER_SYSVIEW_RecordU32(APP_EV_ID(APP_EV_TX_TC), (U32)(id));                     \
        SEGGER_SYSVIEW_MarkStop((unsigned)(id));                                          \
    } while (0)
#define APP_TRACE_WINDOW(v, sc)    app_trace_window((v), (sc))

/* ---- Hafta 3: W3 modulu ---- */
enum
{
    APP_W3_SEND_BEGIN = 0,
    APP_W3_SEND_OK,
    APP_W3_SEND_FAIL,
    APP_W3_RECEIVE,
    APP_W3_DONE,
    APP_W3_COUNT
};

#define APP_W3_MARK_BASE 1000U /* BTN Mark'lari (olay kimligi) ile karismasin */

extern SEGGER_SYSVIEW_MODULE g_app_w3_module;

/* Kosu basi: kuyrugu adlandirir (MsgQ<kapasite>) ve "Kosu ..." yazar. */
void app_trace_q_run_begin(uint16_t run, char variant, bool burst, const void *queue,
                           uint8_t capacity);
void app_trace_q_run_end(uint16_t run);

#define APP_W3_ID(e) (g_app_w3_module.EventOffset + (unsigned)(e))

#define APP_TRACE_Q_SEND_BEGIN(n)                                                         \
    do                                                                                    \
    {                                                                                     \
        SEGGER_SYSVIEW_RecordU32(APP_W3_ID(APP_W3_SEND_BEGIN), (U32)(n));                 \
        SEGGER_SYSVIEW_MarkStart(APP_W3_MARK_BASE + (unsigned)(n));                       \
    } while (0)
#define APP_TRACE_Q_SEND_OK(n, us, q) \
    SEGGER_SYSVIEW_RecordU32x3(APP_W3_ID(APP_W3_SEND_OK), (U32)(n), (U32)(us), (U32)(q))
#define APP_TRACE_Q_SEND_FAIL(n, us)                                                      \
    do                                                                                    \
    {                                                                                     \
        SEGGER_SYSVIEW_RecordU32x2(APP_W3_ID(APP_W3_SEND_FAIL), (U32)(n), (U32)(us));     \
        SEGGER_SYSVIEW_MarkStop(APP_W3_MARK_BASE + (unsigned)(n));                        \
    } while (0)
#define APP_TRACE_Q_RECEIVE(n, lat) \
    SEGGER_SYSVIEW_RecordU32x2(APP_W3_ID(APP_W3_RECEIVE), (U32)(n), (U32)(lat))
#define APP_TRACE_Q_DONE(n)                                                               \
    do                                                                                    \
    {                                                                                     \
        SEGGER_SYSVIEW_RecordU32(APP_W3_ID(APP_W3_DONE), (U32)(n));                       \
        SEGGER_SYSVIEW_MarkStop(APP_W3_MARK_BASE + (unsigned)(n));                        \
    } while (0)
#define APP_TRACE_Q_RUN_BEGIN(run, v, burst, q, cap) \
    app_trace_q_run_begin((run), (v), (burst), (q), (cap))
#define APP_TRACE_Q_RUN_END(run) app_trace_q_run_end(run)

#else /* USE_SYSVIEW */

#define APP_TRACE_BTN_ACCEPT(id, sc) ((void)0)
#define APP_TRACE_BTN_TASK(id)       ((void)0)
#define APP_TRACE_BTN_READY(id)      ((void)0)
#define APP_TRACE_BTN_ENQUEUE(id, q) ((void)0)
#define APP_TRACE_BTN_DROP(id, at)   ((void)0)
#define APP_TRACE_BTN_TX_START(id)   ((void)0)
#define APP_TRACE_BTN_TX_TC(id)      ((void)0)
#define APP_TRACE_WINDOW(v, sc)      ((void)0)

#define APP_TRACE_Q_SEND_BEGIN(n)                    ((void)0)
#define APP_TRACE_Q_SEND_OK(n, us, q)                ((void)0)
#define APP_TRACE_Q_SEND_FAIL(n, us)                 ((void)0)
#define APP_TRACE_Q_RECEIVE(n, lat)                  ((void)0)
#define APP_TRACE_Q_DONE(n)                          ((void)0)
#define APP_TRACE_Q_RUN_BEGIN(run, v, burst, q, cap) ((void)0)
#define APP_TRACE_Q_RUN_END(run)                     ((void)0)

#endif /* USE_SYSVIEW */

#endif /* APP_TRACE_H */

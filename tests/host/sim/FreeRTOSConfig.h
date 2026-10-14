#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* test_queue_exp_sim icin FreeRTOS Windows (MSVC-MingW) simulator ayarlari.
 * Zamanlama davranisi firmware'deki ayarlarla ayni: preemptive, 1 kHz tick,
 * 6 oncelik, yalnizca statik bellek, gorev bildirimleri. */

#define configUSE_PREEMPTION                       1
#define configUSE_TIME_SLICING                     1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION    0
#define configUSE_TICKLESS_IDLE                    0
#define configTICK_RATE_HZ                         ( ( TickType_t ) 1000 )
#define configMAX_PRIORITIES                       6
#define configMINIMAL_STACK_SIZE                   128
#define configMAX_TASK_NAME_LEN                    16
#define configTICK_TYPE_WIDTH_IN_BITS              TICK_TYPE_WIDTH_32_BITS
#define configIDLE_SHOULD_YIELD                    1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES      2 /* 0: kosu basi, 1: Arda cercevesi (frame_timer.h) */
#define configQUEUE_REGISTRY_SIZE                  0
#define configENABLE_BACKWARD_COMPATIBILITY        0
#define configSTACK_DEPTH_TYPE                     uint16_t

#define configUSE_TIMERS                           0
#define configUSE_EVENT_GROUPS                     0
#define configUSE_STREAM_BUFFERS                   0

#define configSUPPORT_STATIC_ALLOCATION            1
#define configKERNEL_PROVIDED_STATIC_MEMORY        1
#define configSUPPORT_DYNAMIC_ALLOCATION           0

#define configUSE_IDLE_HOOK                        0
#define configUSE_TICK_HOOK                        0
#define configUSE_MALLOC_FAILED_HOOK               0
#define configCHECK_FOR_STACK_OVERFLOW             0

#define configGENERATE_RUN_TIME_STATS              0
#define configUSE_TRACE_FACILITY                   0
#define configUSE_STATS_FORMATTING_FUNCTIONS       0
#define configUSE_CO_ROUTINES                      0

#define configUSE_TASK_NOTIFICATIONS               1
#define configUSE_MUTEXES                          0
#define configUSE_RECURSIVE_MUTEXES                0
#define configUSE_COUNTING_SEMAPHORES              0
#define configUSE_QUEUE_SETS                       0

#define INCLUDE_vTaskPrioritySet                   0
#define INCLUDE_uxTaskPriorityGet                  0
#define INCLUDE_vTaskDelete                        0
#define INCLUDE_vTaskSuspend                       1
#define INCLUDE_xTaskDelayUntil                    1
#define INCLUDE_vTaskDelay                         1
#define INCLUDE_xTaskGetSchedulerState             0
#define INCLUDE_xTaskGetCurrentTaskHandle          1
#define INCLUDE_uxTaskGetStackHighWaterMark        1

void vAssertCalled(const char *file, unsigned long line);
#define configASSERT( x )    if( ( x ) == 0 ) vAssertCalled( __FILE__, __LINE__ )

#endif /* FREERTOS_CONFIG_H */

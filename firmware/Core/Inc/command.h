#ifndef COMMAND_H
#define COMMAND_H

#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"

/* Surum komutunun bildirim degeri: CMD_NOTIFY_VARIANT | 'A' (ya da 'B', 'C').
 * Senaryo komutlarinin degerleri 0..0xFF araliginda kalir. */
#define CMD_NOTIFY_VARIANT 0x100U

/* Hafta 3 queue kosusu: CMD_NOTIFY_QRUN | arg (protocol.h CMD_QRUN_*). */
#define CMD_NOTIFY_QRUN    0x200U

/* Gecerli komutlar target gorevine bildirim (notification) degeri olarak
 * iletilir: 0..6 = senaryo, CMD_ARG_DUMP = olcumu bitir, CMD_ARG_QUERY =
 * sorgu, CMD_NOTIFY_VARIANT | surum = surum degistir, CMD_NOTIFY_QRUN | arg
 * = queue kosusu baslat. Scheduler baslamadan once cagrilmalidir. */
void command_init(TaskHandle_t target);

/* USART2 ISR'si her alinan bayt icin cagirir. Komut cercevesini
 * (protocol.h: AA 55 'C'/'V'/'Q' arg checksum) bayt bayt cozer; gecersiz
 * cerceveleri sessizce atar. */
void command_rx_byte_from_isr(uint8_t b, BaseType_t *higher_prio_woken);

#endif /* COMMAND_H */

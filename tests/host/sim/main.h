#ifndef MAIN_H
#define MAIN_H

/* Simulator icin main.h: queue_exp.c'nin kullandigi oncelik ve yigin
 * tanimlari firmware'deki (firmware/Core/Inc/main.h) degerlerle ayni. */
#include <stdint.h>
#include <stdbool.h>

#define PRIO_QEXP_PRODUCER  (tskIDLE_PRIORITY + 2U)
#define PRIO_QEXP_CONSUMER  (tskIDLE_PRIORITY + 1U)
#define PRIO_ARDA_FRAME     (tskIDLE_PRIORITY + 5U)

#define STACK_WORDS_QEXP_PRODUCER  (configMINIMAL_STACK_SIZE * 3U)
#define STACK_WORDS_QEXP_CONSUMER  (configMINIMAL_STACK_SIZE * 3U)
#define STACK_WORDS_ARDA_FRAME     (configMINIMAL_STACK_SIZE * 3U)

/* queue_exp.c doku isteginin yeniden deneme siniri icin */
#define UART_TX_QUEUE_LEN   16U
#define UART_BAUDRATE       230400U

#endif /* MAIN_H */

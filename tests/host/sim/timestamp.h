#ifndef TIMESTAMP_H
#define TIMESTAMP_H

/* Simulator icin zaman damgasi: TIM2 yerine Windows performans sayaci,
 * mikrosaniye, 32 bit (firmware'deki gibi sarar). */
#include <stdint.h>

uint32_t timestamp_now_us(void);

#endif /* TIMESTAMP_H */

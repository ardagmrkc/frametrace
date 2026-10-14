#ifndef FRAME_TIMER_H
#define FRAME_TIMER_H

/*
 * Arda cercevesinin saat kaynagi (arda.h, adim 6).
 *
 * Producer tick'e baglidir (vTaskDelay). Cerceve gorevi de tick'e bagli
 * olsaydi ikisi ayni kenarda uyanir, once yuksek oncelikli cerceve calisir,
 * Producer'in mesaji bir sonraki cerceveyi neredeyse tam bir periyot
 * beklerdi. Bu yuzden cerceve tick'ten ayri bir zamanlayicidan gelir ve
 * tick kenarindan ARDA_FRAME_PHASE_US (yarim cerceve) sonra baslar:
 *
 *   tick (SysTick)  |.........|.........|.........|      Producer uyanir, uretir
 *   cerceve (TIM2)  .....|.........|.........|.....      cerceve gorevi uyanir
 *                   <-500->
 *
 * Kart: TIM2 karsilastirma kanali 1. TIM2 zaten 1 us'lik serbest sayactir
 * (timestamp.c); cerceveler zaman damgalariyla ayni saatte olusur. SysTick
 * (HCLK) ile TIM2 (APB1 x 2) ayni PLL'den beslendigi icin faz kaymaz. Kesme
 * her cercevede karsilastirma degerini ARDA_FRAME_US ilerletir ve cerceve
 * gorevine bildirim verir (bildirim indeksi FRAME_TIMER_NOTIFY_INDEX; 0.
 * indeks kosu baslangicinindir).
 *
 * Simulator (tests/host/sim): kendi uygulamasi tick'e baglidir (faz 0);
 * fazin etkisi yalnizca kartta olculur.
 */
#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"

#define FRAME_TIMER_NOTIFY_INDEX 1U

/* Scheduler oncesi: kesme onceligi ve etkinlestirme. timestamp_init'ten sonra. */
void frame_timer_init(void);

/* Cerceve gorevi, kosu basinda: ilk cerceve son tick kenari + faz anina
 * (gecmisse bir periyot sonrasina) kurulur, kesme acilir. */
void frame_timer_start(TaskHandle_t task);

/* Sonraki cerceveye kadar bloklar. Donus: bu bekleyiste gelen cerceve
 * sayisi (1: zamaninda; n > 1: n - 1 cerceve kacirildi). Saat durmussa
 * configASSERT. */
uint32_t frame_timer_wait(void);

/* Kosu sonu: kesme kapanir, bekleyen bildirim silinir. */
void frame_timer_stop(void);

/* Uygulanan faz (kart: ARDA_FRAME_PHASE_US, simulator: 0) ve olcum: son tick
 * kenarindan bu yana gecen sure (kart: SysTick sayacindan). */
uint32_t frame_timer_phase_us(void);
uint32_t frame_timer_since_tick_us(void);

/* TIM2_IRQHandler (stm32f4xx_it.c) cagirir. */
void frame_timer_isr(void);

#endif /* FRAME_TIMER_H */

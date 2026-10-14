#ifndef QUEUE_EXP_CORE_H
#define QUEUE_EXP_CORE_H

/*
 * queue_exp.c'nin tuketici arayuzu (modul ici). Kosu durumu, Producer ve
 * kayitlar queue_exp.c'dedir; tuketici tarafi degisebilir: A/B/C'de
 * consumer_task (queue_exp.c), Arda'da cerceve gorevi (arda.c). Ikisi de
 * kayitlari yalnizca bu fonksiyonlarla yazar, boylece "alindi / servis
 * bitti" anlami tek yerde tanimlidir.
 *
 * Yalnizca kosunun tuketici gorevi cagirir (tek yazar).
 */
#include <stdbool.h>
#include <stdint.h>
#include "queue_exp.h"

/* Kuyruktan alinan mesaji dogrular: bu kosuya ait mi, sira artiyor mu.
 * FIFO'da kabul edilen mesajlar artan sirayla gelir, basarisiz girisimler
 * araya bosluk birakir. false: sira hatasi sayildi, kaydi yazilmayacak. */
bool qexp_consumer_take(const qexp_message_t *msg);

/* Mesajin servisi bitti. valid ise kaydi yazilir (g2r = start - uretim,
 * servis = done - start); her durumda tuketilen sayisi artar. */
void qexp_consumer_done(const qexp_message_t *msg, bool valid, uint32_t start_us, uint32_t done_us);

/* Alinan mesaj servis edilemedi (ornegin gecersiz tablo indeksi: mesajin
 * kimligi bile bilinmez). Kaydi bos kalir; tuketilen sayilir ki kosu
 * bitebilsin. */
void qexp_consumer_drop(void);

/* Producer son girisimini yapti ve kabul edilen her mesaj tuketildi. */
bool qexp_consumer_finished(void);

/* Kosuyu kapatir: RUN_END izi, DUMPING durumu, UartTxTask'a doku istegi. */
void qexp_finish_run(void);

#endif /* QUEUE_EXP_CORE_H */

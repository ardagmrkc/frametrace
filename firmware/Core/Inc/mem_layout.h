#ifndef MEM_LAYOUT_H
#define MEM_LAYOUT_H

/*
 * Bellek yerlesimi (EWARM/stm32f407vg_flash.icf).
 *
 *   CCM RAM, 0x1000 0000, 64 KB: yalnizca islemcinin veri yolu. DMA
 *     erisemez, kod calistirilamaz; bekleme durumu yok, DMA trafigiyle yol
 *     paylasmaz. Buraya:
 *       - CSTACK (kesmeler ve main), bolgenin en altinda: tasarsa ayrilmis
 *         adres araligina (0x0FFF FFFF) yazar ve HardFault olur; yanindaki
 *         veriyi sessizce bozmaz.
 *       - gorev yiginlari ve TCB'ler
 *       - Arda'nin ic durumu (gelen kutusu, kuryeler, sayaclar)
 *   Ana SRAM, 0x2000 0000, 128 KB: DMA'nin ya da hata ayiklayicinin
 *     okuyacagi her sey ve geri kalan: port mesaj slotlari (gercek hatlarda
 *     DMA okur), SystemView/RTT tamponu, UART ve deney kuyruklari.
 *
 * CCM_DATA, sifirla baslatilan statik bir degiskenin onune yazilir. Bir
 * DMA'nin okuyacagi ya da yazacagi veri asla CCM_DATA olmamalidir. IAR
 * disinda (simulator) bos.
 */
#if defined(__ICCARM__)
#define CCM_DATA _Pragma("location=\".ccmram\"")
#else
#define CCM_DATA
#endif

#endif /* MEM_LAYOUT_H */

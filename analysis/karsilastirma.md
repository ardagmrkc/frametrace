# Kuyruk deneyi: karşılaştırma

`analysis/analyze.py` ile üretildi (2026-10-11T14:36:29+03:00). Kaynak: `measurements/kayit*_meta.json` ve `.csv`. Süreler kartın TIM2 sayacından (1 µs).

## Koşular

| koşu | iz | kapasite / bekleme | kabul | kayıp | art arda kabul / kayıp | kuyruk tepe | başarısız send_call ort. | üretim→alma ort. | p95 | maks | üretici gönderimde | koşu | kontroller |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 01 · A/normal | açık | 4 / 0 tick | 44 | 0 | — | 1/4 | — | 26 µs | 26 µs | 26 µs | 351 µs | 4.319,99 ms | tamam |
| 02 · B/normal | açık | 16 / 0 tick | 44 | 0 | — | 1/16 | — | 26 µs | 26 µs | 26 µs | 352 µs | 4.319,99 ms | tamam |
| 03 · C/normal | açık | 4 / 20 tick | 44 | 0 | — | 1/4 | — | 26 µs | 26 µs | 26 µs | 351 µs | 4.319,99 ms | tamam |
| 04 · A/yoğun | açık | 4 / 0 tick | 24 | 20 | 4 / 20 | 4/4 | 3,1 µs | 5,03 ms | 39,98 ms | 59,97 ms | 242 µs | 2.019,99 ms | tamam |
| 05 · B/yoğun | açık | 16 / 0 tick | 36 | 8 | 16 / 8 | 16/16 | 3,1 µs | 78,32 ms | 279,86 ms | 299,87 ms | 238 µs | 2.019,99 ms | tamam |
| 06 · C/yoğun | açık | 4 / 20 tick | 34 | 10 | 14 / 10 | 4/4 | 19,95 ms | 27,09 ms | 80,07 ms | 80,07 ms | 200,03 ms | 2.219,99 ms | tamam |

## Tek etkenli karşılaştırmalar

### A → B, kapasite 4 → 16 (normal yük): 01 → 02

| ölçü | 01 | 02 | fark |
|---|---|---|---|
| kabul | 44 | 44 | ±0 |
| kayıp | 0 | 0 | ±0 |
| art arda bölümde kayıp | 0 | 0 | ±0 |
| kuyruk tepe doluluğu | 1 | 1 | ±0 |
| üretim → alma, ort. | 26 µs | 26 µs | ±0,0 µs |
| üretim → alma, p95 | 26 µs | 26 µs | ±0,0 µs |
| üretim → alma, maks | 26 µs | 26 µs | ±0,0 µs |
| send_call, kabul ort. | 8,0 µs | 8,0 µs | +0,0 µs |
| üretici gönderimde toplam | 351 µs | 352 µs | +1,0 µs |
| koşu süresi | 4.319,99 ms | 4.319,99 ms | ±0,0 µs |

### A → B, kapasite 4 → 16 (yoğun yük): 04 → 05

| ölçü | 04 | 05 | fark |
|---|---|---|---|
| kabul | 24 | 36 | +12 |
| kayıp | 20 | 8 | −12 |
| art arda bölümde kayıp | 20 | 8 | −12 |
| kuyruk tepe doluluğu | 4 | 16 | +12 |
| üretim → alma, ort. | 5,03 ms | 78,32 ms | +73,29 ms |
| üretim → alma, p95 | 39,98 ms | 279,86 ms | +239,88 ms |
| üretim → alma, maks | 59,97 ms | 299,87 ms | +239,90 ms |
| send_call, kabul ort. | 7,5 µs | 5,9 µs | −1,5 µs |
| send_call, başarısız ort. | 3,1 µs | 3,1 µs | −0,0 µs |
| üretici gönderimde toplam | 242 µs | 238 µs | −4,0 µs |
| koşu süresi | 2.019,99 ms | 2.019,99 ms | ±0,0 µs |

### A → C, bekleme 0 → 20 ms (normal yük): 01 → 03

| ölçü | 01 | 03 | fark |
|---|---|---|---|
| kabul | 44 | 44 | ±0 |
| kayıp | 0 | 0 | ±0 |
| art arda bölümde kayıp | 0 | 0 | ±0 |
| kuyruk tepe doluluğu | 1 | 1 | ±0 |
| üretim → alma, ort. | 26 µs | 26 µs | ±0,0 µs |
| üretim → alma, p95 | 26 µs | 26 µs | ±0,0 µs |
| üretim → alma, maks | 26 µs | 26 µs | ±0,0 µs |
| send_call, kabul ort. | 8,0 µs | 8,0 µs | ±0,0 µs |
| üretici gönderimde toplam | 351 µs | 351 µs | ±0,0 µs |
| koşu süresi | 4.319,99 ms | 4.319,99 ms | ±0,0 µs |

### A → C, bekleme 0 → 20 ms (yoğun yük): 04 → 06

| ölçü | 04 | 06 | fark |
|---|---|---|---|
| kabul | 24 | 34 | +10 |
| kayıp | 20 | 10 | −10 |
| art arda bölümde kayıp | 20 | 10 | −10 |
| kuyruk tepe doluluğu | 4 | 4 | ±0 |
| üretim → alma, ort. | 5,03 ms | 27,09 ms | +22,07 ms |
| üretim → alma, p95 | 39,98 ms | 80,07 ms | +40,09 ms |
| üretim → alma, maks | 59,97 ms | 80,07 ms | +20,10 ms |
| send_call, kabul ort. | 7,5 µs | 17 µs | +9,1 µs |
| send_call, başarısız ort. | 3,1 µs | 19,95 ms | +19,94 ms |
| üretici gönderimde toplam | 242 µs | 200,03 ms | +199,79 ms |
| koşu süresi | 2.019,99 ms | 2.219,99 ms | +200,00 ms |

### İz açık → kapalı (A, yoğun yük): 04 → 07

Kayıt 07 henüz alınmadı.

## Model kontrolü

S: koşunun ölçülen ortalama servis süresi, K: kapasite, W: gönderme beklemesi. A/B'de art arda bölüm boş bir tüketiciye gelir: K. mesaj önündeki K − 1 mesajın servisini bekler. C'de bloklanan gönderim tüketici bir mesajı servise aldığı anda geçer: önünde K − 1 mesaj ve serviste bir mesaj vardır.

| koşu | model | model değeri | ölçülen (maks / ort.) | sapma |
|---|---|---|---|---|
| 04 · A/yoğun | (K − 1)·S = 3 × 19,97 ms | 59,90 ms | 59,97 ms | 0,1 % |
| 05 · B/yoğun | (K − 1)·S = 15 × 19,98 ms | 299,65 ms | 299,87 ms | 0,1 % |
| 06 · C/yoğun | K·S = 4 × 19,97 ms | 79,87 ms | 80,07 ms | 0,3 % |
| 06 · C/yoğun | başarısız send_call ≈ W = 20 tick | 20,00 ms | 19,95 ms | −0,3 % |

Gözlenen maksimum bir worst-case garantisi değildir; bu yük tarifi ve bu kartta ölçülen değerdir.

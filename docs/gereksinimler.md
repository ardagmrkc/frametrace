# FrameTrace · Gereksinimler

[English](requirements.md) · [Tasarım notları](design.md) · [README](../README.tr.md)

| Sürüm | Tarih | Not |
|---|---|---|
| 0.1 | 2026-10-13 | İlk taslak. Kararlar: DL-04 ≤ 20 ms; RAM bütçesi uygulama ve ölçüm altyapısı olarak ikiye bölündü. |
| 0.2 | 2026-10-13 | Port katmanı (queuing + sampling) ve sağlık izleyici (bölüm 6). KAY-04 uygulandı; servis başı damgası düzeltildi (DL-05). |
| 0.4 | 2026-10-13 | 4. adım: ikinci kurye = ikinci hat (bölüm 7). |
| 0.5 | 2026-10-13 | 5. adım: toplu servis (KAN-04). DL-05 ve KAN-03 toplu servise göre yeniden yazıldı (DL-05b, KAN-03b). |
| 0.6 | 2026-10-13 | 6. adım: çerçeve TIM2'den, tick kenarından 500 µs sonra (BUT-07). |
| 0.7 | 2026-10-13 | Bellek yerleşimi (RAM-05): yığınlar ve çerçeve görevinin durumu CCM'de. |
| 0.8 | 2026-10-13 | RAM hesabı: iz kapalı derleme ölçüldü. |
| 1.0 | 2026-10-14 | Kayıt 14 (adım 1–6) izinden değerlendirildi; durumlar güncellendi. |

Bu belge çerçeve tüketicisinin (izlerdeki görev `Arda`) gereksinimlerini tanımlar. Adımlar üst üste eklendi:
1. 1 ms'lik çerçeve görevi, 1 baytlık tutamaç, 0 tick bekleme, derleme anı boyut hesabı.
2. Servis = 15 baytlık iletim modeli.
3. Port katmanı ve deadline sayacı.
4. İkinci hat.
5. Toplu servis.
6. TIM2 fazlı çerçeve.

Kartta ölçülen kayıtlar 08 (1. adım), 09 (1–2) ve 14 (1–6); 3.–5. adımlar host simülasyonunda doğrulandı. Kuyruk
tabanlı taban çizgisi tüketicileri (A/B/C, kayıt 01–07) bu belgenin kapsamı dışında; yalnızca karşılaştırma için
kullanılıyor ve değiştirilmiyor (GEN-01).

Her gereksinim ölçülebilir bir değer, bir gerekçe ve bir doğrulama yolu taşır. Bunlardan biri eksik olan satır
gereksinim sayılmaz. Bir gereksinim değişirse yeni satır eklenir; eski satır silinmez, değiştiği belirtilir.

## 1. Kapsam ve varsayımlar

- Kart: STM32F407VG, 168 MHz. FreeRTOS V11, tick 1 kHz (1 tick = 1 ms), yalnızca statik bellek.
- Zaman damgaları TIM2'den, 1 µs çözünürlükte.
- UART (230 400 baud) yalnızca komutları ve koşu sonrası dökümü taşır; ölçüm boyunca sessizdir.
- Servis bir **modeldir**: 12 baytlık mesajın 15 baytlık ikili çerçevede iletim süresi. Koşu sırasında hatta veri
  gönderilmez; ikinci hat ve DMA'lı toplu sefer de modeldir.
- Ölçümler tek tekrara dayanır, iz açıktır. Kayıt 14'ün dökümü kaydedilmedi; değerleri SystemView izinin
  çözülmesinden gelir (`analysis/systemview/14Arda_yogun_2_v2.SVDat`).

## 2. Tanımlar

| Terim | Tanım | Kayıttaki alan |
|---|---|---|
| Üretim anı | Producer'ın gönderme çağrısından hemen önce aldığı zaman damgası | `t_gen_us` |
| Alınma | Üretimden, çerçeve görevinin tutamacı mesaj portundan aldığı ana kadar | `intake_max_us` (sayaç) |
| Servis başı (g2r) | Üretimden mesajın servisinin başlamasına kadar (gelen kutusunda bekleme dahil). Seferdeki *k*. mesaj için: seferin başladığı çerçevenin başı + *k* × 651 µs | `generation_to_receive_us` |
| Servis | Servis başından seferin bittiği çerçeveye kadar | `service_us` |
| Art arda bölüm | Yoğun yükte 11–34 arası, aralarında bekleme olmadan gönderilen 24 girişim | `phase = art_arda` |
| Çerçeve | Çerçeve görevinin bir uyanışı (TIM2, 1 ms) | `frames` |
| Çerçeve işi | Bir çerçevenin uyanıştan bitişe kadar süren işi | `frame_work_max_us` |

**Doğrulama yöntemleri:** **A** analiz (hesap, derleme anı kontrolü) · **T** test (host testi, gerçek çekirdekli
simülasyon, hata enjeksiyonu) · **Ö** kart ölçümü (döküm ya da iz) · **İ** inceleme (kod okuma).

**Durum:** ✅ sağlanıyor · ❌ sağlanmıyor · ⏳ henüz doğrulanmadı

## 3. Genel (GEN)

| Kimlik | Gereksinim | Gerekçe | Doğrulama | Durum |
|---|---|---|---|---|
| GEN-01 | Taban çizgisinin (A/B/C) kodu, yükü ve ölçüm tanımları değiştirilmemeli. | Karşılaştırmanın referansı sabit kalsın. | İ, T (simülasyonda A/B/C'nin kayıp kümeleri aynı) | ✅ |
| GEN-02 | Çerçeve tüketicisi, taban çizgisiyle aynı yük tarifini ve aynı 12 baytlık mesajı kullanmalı. | Karşılaştırma tek etkenli kalsın. | İ | ✅ |
| GEN-03 | Her adımda tek bir etken değişmeli ve her adımın ayrı bir ölçümü olmalı. | Bir sonucun hangi değişiklikten geldiği ayırt edilebilsin. | İ | ✅ kart: 08 → 09 yalnızca servis modeli; 3.–5. adımlar simülasyonda |
| GEN-04 | Her ölçüm dosyası, alındığı firmware sürümünü kendisi göstermeli. | Bir ölçümün hangi yazılımla alındığı dosyanın kendisinden okunabilsin. | İ | ❌ |

## 4. Yük sözleşmesi (YUK)

| Kimlik | Gereksinim | Değer | Gerekçe | Doğrulama | Durum |
|---|---|---|---|---|---|
| YUK-01 | Bir koşuda gönderme girişimi sayısı | 44 | Yük sözleşmesi. | A (`QEXP_ATTEMPTS`), Ö (`attempted`) | ✅ 08, 09, 14: 44 |
| YUK-02 | Art arda bölümdeki en çok mesaj | 24 | Port kapasitesi bu değerden hesaplanır (YUK-05). | A (`arda.h` kontrol b), Ö | ✅ 24 mesaj; 09'da 323 µs, 14'te ~670 µs içinde |
| YUK-03 | Normal girişimler arası bekleme | ≥ 100 ms | Art arda bölümden önce sistem boş olsun. | A (`arda.h` kontrol a) | ✅ |
| YUK-04 | Mesaj boyu | 12 bayt | Mesaj biçimi sabit: sıra no, üretim zamanı, koşu no. | A (`queue_exp.h` derleme anı kontrolü) | ✅ |
| YUK-05 | Art arda bölümden sonra servis gelişten hızlı olmalı: her 100 ms'de gelen 1 mesaja karşılık en az 2 mesaj servis edilmeli. | > 1 | Birikme yalnızca azalsın; yoksa port zamanla dolar. | A (`arda.h` kontrol c) | ✅ |
| YUK-06 | Sözleşme dışı yükte davranış tanımlı olmalı: fazla mesaj `pool_empty` olarak sayılır, gönderim başarısız döner, sistem takılmaz, koşu biter ve kontroller kırmızıya döner. | — | Gerçek bir sistemde yük dışarıdan gelir ve sözleşmeyi aşabilir. | T, Ö (32 ve 33'lük art arda bölümle stres testi) | ⏳ |

## 5. Kayıp politikası (KAY)

Mesajlar **olay verisidir** (ARINC 653'teki queuing port karşılığı): her biri önemlidir, sırası korunur, eskiyen mesaj
atılmaz.

| Kimlik | Gereksinim | Değer | Gerekçe | Doğrulama | Durum |
|---|---|---|---|---|---|
| KAY-01 | Yük sözleşmesi içinde kaybolan mesaj (gönderimde ve serviste) | 0 | Olay verisi geri getirilemez. | T, Ö (kontrol "hiç kayıp yok"; iz: SEND = RECEIVE = DONE) | ✅ 08: 0, 09: 0, 14: 0 (44/44/44) |
| KAY-02 | Oluşan her kayıp bir sayaçta ve kontrol listesinde görünmeli; sessiz kayıp olmamalı. | — | Hata, oluştuğu yerde yakalanmazsa sonradan bulunamaz. | T (hata enjeksiyonu: bozuk tutamaç → 4 kontrol kırmızı) | ✅ simülasyon |
| KAY-03 | Veri yolundaki her kuyruk ve port işlemi 0 tick beklemeli. Dolu bulunan kuyruk tasarım hatasıdır: ya derleme anında engellenir ya `configASSERT` ile durulur ya da sayılır. | 0 tick | Bekleme kontrol aracı olarak kullanılırsa tick yarışları doğar (taban çizgisi C). | İ, Ö (`send_call` 7–8 µs) | ✅ |
| KAY-04 | Yaş sınırını aşan mesaj atılmamalı, "deadline kaçtı" olarak raporlanmalı. | — | KAY-01 ile çelişmesin. | T, Ö (`deadline_miss` = kayıtlarda g2r > 20 ms olan mesaj sayısı) | ✅ simülasyon (sayaç = kayıt); 14: izde 20 ms'yi aşan yok |
| KAY-05 | Döküm isteği 0 beklemeyle konmalı; yer yoksa en çok 90 tick yeniden denenmeli, aşılırsa `configASSERT`. | 90 tick | UART kuyruğunun en kötü boşalma süresi 45 ms'nin iki katı. | A (`queue_exp.c`), Ö (`dump_retry`) | ✅ 08, 09: 0 |

## 6. Port katmanı (PRT)

Görevler veriyi yalnızca portlarla değiştirir (`port_layer.h`, tablo `port_config.c`). Mesajlar **queuing port**tan
geçer (`PORT_ARDA_MSG`). Çerçeve görevinin son durumu (birikme, en eski mesajın yaşı, deadline sayısı) **sampling
port**tan geçer (`PORT_ARDA_STATUS`): okunmayan güncelleme atlanır (hata değil), iki çerçeveden eski değer bayattır.
Hatalar sağlık izleyiciye (`health.h`) bildirilir; her olay türünün tepkisi bir tabloda (yetkisiz erişim: dur,
diğerleri: say).

| Kimlik | Gereksinim | Değer | Gerekçe | Doğrulama | Durum |
|---|---|---|---|---|---|
| PRT-01 | Her portun tek yazarı ve tek okuyucusu olmalı; başka görevin erişimi sistemi durdurmalı. | — | Veri sahipliği koddan değil yapılandırmadan okunabilsin. | İ (`port_bind`, `check_access`, `HM_PORT_ACCESS`) | ✅ |
| PRT-02 | Queuing port: gönderilen = alınan, taşma 0, geri dönmeyen tutamaç 0 | 0 | KAY-01'in port düzeyindeki karşılığı. | Ö (`pool_*`), T (simülasyon: port 44/44) | ✅ simülasyon; 14: izde 44 gönderim, 44 alma, 44 teslim |
| PRT-03 | Sampling port: okunan değerin yaşı en çok iki çerçeve; bayat okuma 0 | ≤ 2 ms, 0 | Durum verisi ancak tazeyse anlamlıdır. | Ö (`status_age_max_us`, `status_stale`) | ✅ simülasyon (en çok 136 µs, 0) · kart ⏳ |
| PRT-04 | Port çağrıları beklememeli; sampling port kopyası kritik bölümde ve en çok 32 bayt olmalı. | 0 tick, ≤ 32 B | Kritik bölüm kısa ve sabit süreli kalsın. | A (`port_config.c` `_Static_assert`), İ | ✅ durum mesajı 16 B |

## 7. Kanallar (KAN)

Kurye bir hattı modeller: bir sefer, mesajların hattan iletimidir ve tek UART hattı aynı anda tek çerçeve taşır. İki
hat **kapasite** için kullanılır, yedeklilik için değil.

| Kimlik | Gereksinim | Değer | Gerekçe | Doğrulama | Durum |
|---|---|---|---|---|---|
| KAN-01 | Her kurye ayrı bir hattı modellemeli; kurye sayısı kartta boştaki UART sayısını aşmamalı. | ≤ 5 (USART1/3/6, UART4/5) | Tek hatta birden çok kurye saymak modeli gerçek dışı yapar. | A (`arda.h` `_Static_assert`) | ✅ |
| KAN-02 | Dökümde kurye sayısı ve aynı anda yolda olan en çok kurye bulunmalı; kayıt, kartın kurye sayısıyla uyuşmalı. | 2 / 2 | Kayıt yanlış firmware ile alınırsa anlaşılsın. | Ö (`couriers`, `courier_busy_max`) | ✅ simülasyon; 14: izde her ağır çerçevede iki hattın teslimi |
| KAN-03 | Servisler başladıkları sırayla bitmeli. | — | Alıcı iki hattı sıra numarasıyla birleştirir. | İ | 0.5'te KAN-03b ile değişti (toplu seferde yükün tamamı aynı anda teslim sayılır) |
| KAN-03b | Mesajların servis başları sıra numarasını izlemeli (eşit anlar olabilir). | 0 ters | Başlangıç sırası bozulmazsa alıcıya birleştirme için en çok bir mesajlık tampon yeter. | İ (sırayla dağıtım), Ö (kontrol "servis başları sıra numarasını izledi") | ✅ simülasyon (0 ters); 14: izde teslimler sıra numarasıyla |
| KAN-04 | Toplu sefer: bir seferde en çok 3 mesaj; mesajlar aynı hatta arka arkaya, her biri kendi 15 baytlık çerçevesinde gider; kurye yükün dolmasını beklemez. | ≤ 3 | Mesaj biçimi ve yük değişmeden 1 ms'ye yuvarlamada boşa giden çerçeve sonu kullanılsın (3 mesaj 1 953 µs, 2 çerçeve). | A (`ARDA_BATCH_MAX`), Ö (`batch`, `batch_max`) | ✅ simülasyon (3 / 3); 14: izde 2 ms'de bir 6 teslim (2 hat × 3) |

## 8. Zamanlama (DL)

| Kimlik | Gereksinim | Değer | Gerekçe | Doğrulama | Durum |
|---|---|---|---|---|---|
| DL-01 | Alınma, en kötü durumda | ≤ 1,1 ms | Bir çerçeve periyodu artı sapma payı. | Ö (`intake_max_us`; izde RECEIVE gecikmesi) | ✅ 08: 990 µs, 09: 983 µs, 14: 697 µs |
| DL-02 | Normal yükte servis başı (g2r), en kötü durumda | ≤ 1,1 ms | Normal yükte mesaj beklememeli. | Ö | ✅ 09: 988 µs, 14: ≈ 0,48 ms · ❌ 08 (DL-03 yüzünden) |
| DL-03 | Art arda bölümün etkisi sonraki normal mesajlara sızmamalı: art arda bölümden sonraki ilk normal mesajın (35) servis başı | ≤ 1,1 ms | Bir yük piki başka trafiği etkilerse hata zincirleme yayılır. | Ö | ✅ 09: 988 µs, 14: ≈ 0,48 ms · ❌ 08: 381 ms |
| DL-04 | Art arda bölümde servis başının (g2r) en kötü değeri | **≤ 20 ms** | Bir servis periyodu (20 ms, 50 Hz'lik bir alıcı). | Ö (`g2r_max_us`; iz) | ✅ 14: 6,86 ms · ❌ 09: 23,7 ms · ❌ 08: 460,7 ms |
| DL-05 | Servis süresi modele uymalı: sabit modelde 20 ms, iletim modelinde bir çerçeve (1 ms). | ± 50 µs | Gecikme hesabı servis süresine dayanır. | Ö (`service_us`) | 08, 09: 43 mesajda ✅, ilk art arda mesajda (11) 142 µs kısa: damga artefaktı, 0.2'de düzeltildi. 0.5'te DL-05b ile değişti. |
| DL-05b | Servis süresi modele uymalı: *n* mesajlık seferdeki *k*. mesajın servisi ⌈*n* × 651 / 1000⌉ ms − *k* × 651 µs (n = 1: 1 ms; n = 3: 2,0 / 1,35 / 0,70 ms). | ± 50 µs | Toplu seferde süre mesajın konumuna bağlıdır. | Ö (`service_us`) | ✅ simülasyon (0,67–2,04 ms, Windows zamanlaması) · kart ⏳ (servis başı izde ayrıca damgalanmıyor; döküm gerekir) |
| DL-06 | Ölçülen gecikmeler, adımdan önce yazılan tahminle uyuşmalı. | fark ≤ %5 | Tahmin tutmuyorsa sistem anlaşılmamıştır. | A + Ö | ✅ 08: tahmin 463, ölçüm 460,7 ms · ✅ 09: 23,9 / 23,7 ms · ❌ 14: 7,6 / 6,86 ms (−%10); tahmin, art arda bölümün ilk çerçeveden önce bittiğini varsaydı, bölüm artık çerçeveye bölünüyor |

## 9. Çerçeve bütçesi (BUT)

| Kimlik | Gereksinim | Değer | Gerekçe | Doğrulama | Durum |
|---|---|---|---|---|---|
| BUT-01 | Ardışık iki çerçeve başlangıcı arası | 1 ms ± 10 µs | Zamanlama önceden bilinsin. | Ö (`frame_dt_min/max_us`; iz) | ✅ 08, 09: 998–1 002 µs · 14: 999,90–1 000,18 µs |
| BUT-02 | Bir çerçevenin işi, en kötü durumda | ≤ 300 µs (%30) | Kesmelere ve eklenecek işe pay kalsın. | Ö (`frame_work_max_us`; iz) | ✅ 09: 143 µs · 14: 292,4 µs (SystemView çubuğu), sınıra yakın |
| BUT-03 | Kaçırılan çerçeve | 0 | Kaçırılan çerçeve, zamanlama varsayımının çöktüğünü gösterir. | Ö (`frame_miss`; iz) | ✅ 08, 09: 0 · 14: izde 1,0002 ms'den uzun aralık yok |
| BUT-04 | Bir çerçevede işlenen en çok mesaj | ≤ K | Çerçeve işi yükten bağımsız sınırlansın. K, mesaj başına ölçülen süreden hesaplanır (iz açık ~13 µs). | A, Ö | ⏳ |
| BUT-05 | Çerçeve görevi sistemin en yüksek önceliğinde (P5) tek başına olmalı. | P5 | Çerçeveyi yalnızca kesmeler geciktirebilsin. | İ (`main.h`) | ✅ |
| BUT-06 | Seferin süresi çerçeve periyodunun tam katı olmalı. | — | Aksi halde sefer bir sonraki çerçeveye yuvarlanır ve servis farkında olunmadan uzar. | A (`arda.c` derleme anı kontrolü) | ✅ |
| BUT-07 | Çerçeve, Producer'ın uyandığı tick kenarından yarım periyot sonra başlamalı (üretici ile tüketici aynı kenara bağlı olmamalı). | 500 µs, sapma ≤ 50 µs | Aynı kenarda önce yüksek öncelikli çerçeve çalışır ve mesaj neredeyse bir periyot bekler (988 µs). Yarım faz bu beklemeyi yarıya indirir, iki görevi kenarda yarıştırmaz. | A (`frame_timer.c`), Ö (`frame_phase_min/max_us`; iz) | ✅ 14: 498,9–500,1 µs (2 051 çerçeve, SysTick → TIM2 kesmesi) |

## 10. Bellek (RAM)

Uygulama ile ölçüm aracı ayrı bütçelenir. SystemView'in kayıt tamponu ölçüm aracının belleğidir; iz kapalı
derlemede tamamen çıkar. İki bölge ayrı raporlanır (`mem_layout.h`, `stm32f407vg_flash.icf`):

- **CCM RAM** (0x1000 0000, 64 KB): yalnızca işlemcinin veri yolu. DMA erişemez, kod çalıştırılamaz. Buraya
  yalnızca işlemcinin verisi konur.
- **Ana SRAM** (0x2000 0000, 128 KB): DMA'nın ya da hata ayıklayıcının okuyabileceği her şey ve geri kalan.

| Kimlik | Gereksinim | Değer | Gerekçe | Doğrulama | Durum |
|---|---|---|---|---|---|
| RAM-01a | Uygulamanın RAM'i (iz kapalı derleme) | ≤ 32 KB | Uygulama büyüdükçe pay kalsın. | A (iz kapalı derlemenin map dosyası) | ✅ 19 706 B (ana SRAM 7 286 + CCM 12 420) |
| RAM-01b | Ölçüm altyapısının RAM'i (SystemView + RTT) | ≤ 20 KB, düşen olay 0 | Ölçüm aracı sonucu bozmasın, olay kaçırmasın. | A (map), Ö (izdeki OVERFLOW paketleri) | ✅ 18 085 B; 06 ve 14 izlerinde düşen olay 0 |
| RAM-02 | Çerçeve tüketicisinin RAM'i, yığın dahil (port katmanı, sağlık izleyici, çerçeve saati) | ≤ 4 KB | Tüketici küçük kalsın. | A (map) | ✅ 2 568 B |
| RAM-03 | Her görev yığınının boyutu, statik analizle ölçülen kullanımın büyüğünün en az 1,25 katı olmalı ve gerekçesi yazılmalı. | ≥ 1,25 × | Yığın boyutu tahminle değil, kanıtla seçilsin. | A (IAR yığın analizi), Ö (`stack_hwm_*`) | ⏳ (bugün 3–10 kat pay; aşağıdaki tablo) |
| RAM-04 | Dinamik bellek kullanılmamalı. | — | Bellek kullanımı derleme anında bilinsin. | İ (`configSUPPORT_DYNAMIC_ALLOCATION 0`, map'te `malloc` yok) | ✅ |
| RAM-05 | DMA'nın erişebileceği veri ana SRAM'de, yalnızca işlemcinin verisi CCM'de olmalı. Ana yığın CCM'in en altında olmalı: taşma ayrılmış adrese yazar ve HardFault olur. CCM'deki değişkenler açılışta sıfırlanmalı. | — | Taşma sessiz kalmasın; DMA'nın göremeyeceği yere DMA verisi konmasın. | A (map: CSTACK 0x1000 0000–0x1000 0800, `.ccmram` türü "zero"), T (`CSTACK_OVERFLOW_TEST` → HardFault) | Map ✅ · taşma testi kart ⏳ |

**Bölgelere göre döküm** (Debug, map dosyası):

| Derleme | Ana SRAM (128 KB) | CCM (64 KB) | Toplam (192 KB) |
|---|---|---|---|
| İz açık | 25 375 B dolu · %80,6 boş | 12 420 B dolu · %81,0 boş | 37 795 B dolu · %80,8 boş |
| İz kapalı | 7 286 B dolu · %94,4 boş | 12 420 B dolu · %81,0 boş | 19 706 B dolu · %90,0 boş |

Ana SRAM'in iz açık dökümü (bayt):
- SystemView + RTT: 18 085;
- ölçüm zincirinden kalan buton ve telemetri tamponları: 2 432;
- taban çizgisi ve kayıtlar: 1 532;
- UART gönderim kuyruğu: 1 200;
- FreeRTOS çekirdeği (Idle yığını 512 dahil): 861;
- port katmanı, sağlık izleyici, çerçeve saati ve çerçeve görevinin geri kalanı: 788;
- diğer: 477.

CCM (bayt): ana yığın 2 048 (en altta), 6 görev yığını 9 728, 6 TCB 480, çerçeve görevinin durumu 164.

**Görev yığınları: ayrılan / gereken** (IAR yığın analizi, en kötü çağrı zinciri, bayt). İz açık değerleri
SystemView'in özyinelemesi yüzünden üst sınırdır. Analiz, görev geçişinde ve kesme girişinde yığına yazılan
çerçeveyi (FPU ile en çok ~200 B) içermez.

| Görev | Ayrılan | Gereken, iz kapalı | Gereken, iz açık |
|---|---|---|---|
| UartTx | 2 048 | 736 | ≤ 1 048 |
| Telemetry | 1 536 | 232 | ≤ 576 |
| Producer | 1 536 | 152 | ≤ 504 |
| Consumer | 1 536 | 152 | ≤ 504 |
| Button | 1 536 | 136 | ≤ 488 |
| Çerçeve görevi (Arda) | 1 536 | 160 | ≤ 512 |
| Kesmeler (ana yığın) | 2 048 | en çok 288, hepsi iç içe 512 | en çok 600, hepsi iç içe 2 136 |

## 11. İzlenebilirlik

| Kimlik | Uygulayan | Doğrulayan | Durum |
|---|---|---|---|
| GEN-01 | — | Simülasyon: A/B/C kayıp kümeleri | ✅ |
| GEN-04 | — | Ölçüm dosyasındaki sürüm alanı | ❌ |
| YUK-02, YUK-03, YUK-05 | `arda.h` derleme anı kontrolleri (a), (b), (c) | Derleme; bozulmuş yükle derleme testi | ✅ |
| YUK-04 | `queue_exp.h` `_Static_assert(sizeof(qexp_message_t) == 12U)` | Derleme | ✅ |
| YUK-06 | `port_send` → `pool_empty` (port taşması) | Stres testi | ⏳ |
| KAY-01 | `arda.h` kuralları K1–K7, `port_layer.h` kuralları P1–P6 | Kontrol "hiç kayıp yok" (Python, arayüz), simülasyon, iz | ✅ |
| KAY-02 | Sayaçlar `pool_*` (port + sağlık izleyici), `dump_retry`; `qexp_consumer_drop` | Hata enjeksiyonu: bozuk tutamaç (20. gönderim) → 4 kontrol kırmızı | ✅ simülasyon |
| KAY-03 | `port_send`, `port_take`, `port_release`, `port_read/write`: hepsi 0 tick; `configASSERT` | İnceleme; `send_call` ölçümü | ✅ |
| KAY-04 | `arda.c` `dispatch` (K7) → `deadline_miss`, `HM_DEADLINE_MISS` | Kontrol "deadline sayacı kayıtlarla tutarlı"; simülasyon | ✅ |
| KAY-05 | `qexp_finish_run`, `QEXP_DUMP_RETRY_TICKS` | Kontrol "döküm isteği beklemeden kondu" | ✅ |
| PRT-01…PRT-04 | `port_layer.c`, `port_config.c`, `health.c`; bağlama `queue_exp_init` | Simülasyon; "durum portu …" kontrolleri | Bölüm 6 |
| KAN-01…KAN-04 | `arda.h` (`ARDA_COURIERS`, `ARDA_BATCH_MAX`), `arda.c` `dispatch` (sırayla dağıtım), `complete_trips` | Simülasyon; "kurye", "seferde en çok 3", "sıra numarası" kontrolleri; kayıt 14 izi | Bölüm 7 |
| DL-01…DL-05b | `arda.c` çerçeve sırası (bitir → al → dağıt → durum yaz); servis başı = çerçeve başı + *k* × 651 µs | Kayıt 08, 09 (döküm), 14 (iz); arayüz kutucukları | Bölüm 8 |
| BUT-01, BUT-03, BUT-07 | `frame_timer.c` (TIM2 CC1 kesmesi, SysTick'e göre faz), `arda.c` çerçeve döngüsü | `frame_dt_*`, `frame_miss`, `frame_phase_*`; kayıt 14 izi | ✅ |
| BUT-02, BUT-04 | `arda.c` | `frame_work_max_us`; iz | BUT-02 ✅ · BUT-04 ⏳ |
| BUT-06 | `arda.c` `_Static_assert(ARDA_TRIP_TICKS_FIXED % ARDA_FRAME_TICKS == 0U)` | Derleme | ✅ |
| RAM-01a, RAM-01b, RAM-02 | — | Map dosyası; izdeki OVERFLOW paketleri | Bölüm 10 |
| RAM-04 | `FreeRTOSConfig.h` | Map dosyası | ✅ |
| RAM-05 | `mem_layout.h` (`CCM_DATA`), `stm32f407vg_flash.icf` (`place at start of CCMRAM_region { block CSTACK }`), `main.c` (`CSTACK_OVERFLOW_TEST`) | Map dosyası; taşma testi derlemesi | Map ✅ · kart ⏳ |

## 12. Açık işler

| Konu | Not |
|---|---|
| Çerçeve başına mesaj sınırı (BUT-04) | Kayıt 14 izinden: mesaj başına ~13 µs (iz açık). 32 slotun hepsini alan bir çerçeve ~450 µs sürerdi. |
| Kesme yığını kuralı (RAM-03) | IAR'ın kesme "toplam kullanımı" bütün kesmelerin iç içe geçtiğini varsayar (2 136 B). Öncelik 5'teki EXTI0, USART2 ve TIM2 birbirini kesemez, yalnızca öncelik 15'teki SysTick/PendSV'yi keser: ≤ 2 × 600 = 1 200 B. `stack_usage.suc`'a `check that` kuralı olarak yazılacak. Ana yığın CCM'in tabanında, taşma HardFault üretir (RAM-05). |
| Kayıt 14'ün dökümü | Adım 1–6 için kartın kendi sayaçlarıyla (deadline, faz, port, durum, kurye, sefer) kaydedilmiş bir koşu; iz kapalı bir koşu da. |
| Alıcıda iki hattın birleştirilmesi | Model dışı. Gerçek sistemde sıra numarasıyla birleştirme ve bir hat koptuğunda "kanal düştü" sağlık olayı gerekir; tek hatta kapasite yarıya iner ve deadline kaçmaları geri gelir. |
| Firmware sürüm kimliği (GEN-04) | Her dökümde ve izde. |
| Stres testi (YUK-06) | 32 ve 33 mesajlık art arda bölüm. |
| MPU | Çerçeve görevinin verisini yerleşimle değil donanımla ayırmak. |
| Ölçüm zincirinden kalan tamponlar (2 432 B) | Buton ve telemetri ölçümünden kalıyor; bu deneyde kullanılmıyor. |

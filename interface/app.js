"use strict";

/*
 * Hafta 3 queue deneyi kayit masasi (odev-03). Web Serial ile karta baglanir,
 * kayit matrisindeki kosulari baslatir, kosu bitince dokulen satirlari toplar
 * ve analysis/queue_experiment.py ile ayni kontrolleri yapip ayni dosyalari
 * indirir (kayitNN_..._{.csv,_counters.csv,_raw.txt,_meta.json}).
 *
 * Hat bicimi (firmware/Core/Inc/protocol.h): her satir ASCII metindir,
 * bosluklarla 63 bayta tamamlanir, 64. bayt LF'dir.
 *   QRUN,<kosu>,<surum>,<yuk>,<kapasite>,<bekleme_tick>,<servis_ms>,<aralik_ms>,<iz>
 *   QBUSY,<kosu>
 *   QMSG,<kosu>,<seq>,<ok|fail>,<t_us>,<send_us>,<g2r_us>,<svc_us>,<doluluk>
 *   QCNT,<kosu>,<ad>,<deger>
 *   QEND,<kosu>,<QMSG_sayisi>
 *   ACK,<seq>,S<n>,<surum>      surumun sonundaki T: SystemView izlemesi acik
 * Hafta 2'nin TEL/BTN (kisa ikili cerceve dahil) ve REC/CNT/END satirlari
 * cozulur ama yok sayilir.
 *
 * Komutlar (5 bayt, AA 55 tur arg checksum): baglaninca durum sorgusu
 * 'C' FF (kart ACK ile yanit verir); kosu 'Q' arg (bit 0..1 surum, bit 4 yogun).
 * Kosu sirasinda hatta trafik yoktur.
 */

const LINE_SIZE = 64;
const LF = 0x0a;
const BIN_SYNC = 0xa5;
const BIN_SIZE = { 0x54: 19, 0x42: 20 }; // 'T' (TEL), 'B' (BTN)

// Kartin UART_BAUDRATE degeriyle (firmware/Core/Inc/main.h) ayni olmali.
const BAUD_RATE = 230400;

const CMD_SYNC0 = 0xaa;
const CMD_SYNC1 = 0x55;
const CMD_TYPE_SCENARIO = 0x43; // 'C'
const CMD_TYPE_QRUN = 0x51; // 'Q'
const CMD_ARG_QUERY = 0xff;
const QRUN_LOAD_BURST = 0x10;

// queue_exp.h
const ATTEMPTS = 44;
const BURST_FIRST = 11;
const BURST_LAST = 34;
const DEADLINE_MS = 20; // arda.h ARDA_DEADLINE_MS (DL-04): uretimden servis baslangicina
const DEADLINE_US = DEADLINE_MS * 1000;
const ARDA_COURIERS = 2; // arda.h ARDA_COURIERS (adim 4: kurye = hat)
const ARDA_BATCH = 3; // arda.h ARDA_BATCH_MAX (adim 5: toplu servis)
const ARDA_FRAME_PHASE_US = 500; // arda.h ARDA_FRAME_PHASE_US (adim 6: cerceve tick kenarindan bu kadar sonra)
const FRAME_PHASE_TOL_US = 50; // olculen faz: ayar .. ayar + 50 us

const ACK_TIMEOUT_MS = 1500;
const QRUN_TIMEOUT_MS = 2000;
const RUN_TIMEOUT_MS = 20000;
// Ilerleme cubugu icin tahmin: 50 ms bekleme + girisim aralari + son servis.
const EXPECTED_RUN_MS = { normal: 4400, yogun: 2100 };
const STORAGE_KEY = "hafta3-queue-masasi-v1";

const RECORDS = [
  { id: "01", variant: "A", load: "normal", trace: true },
  { id: "02", variant: "B", load: "normal", trace: true },
  { id: "03", variant: "C", load: "normal", trace: true },
  { id: "04", variant: "A", load: "yogun", trace: true },
  { id: "05", variant: "B", load: "yogun", trace: true },
  { id: "06", variant: "C", load: "yogun", trace: true },
  { id: "07", variant: "A", load: "yogun", trace: false },
  // Arda/yogun: odevin A/B/C matrisinin disinda, kendi tasarimimiz (ek deney). Adimlar ust uste
  // eklendi; kayit 14 altisinin toplamidir (index.html adim listesi). Ara adimlarin kayitlari kapali:
  // { id: "08", variant: "D", load: "yogun", trace: true, arda: true }, // adim 1
  // { id: "09", variant: "E", load: "yogun", trace: true, arda: true }, // adim 1 + 2
  // { id: "10", variant: "D", load: "yogun", trace: true, arda: true }, // adim 1 + 3
  // { id: "11", variant: "E", load: "yogun", trace: true, arda: true }, // adim 1 + 2 + 3
  // { id: "12", variant: "E", load: "yogun", trace: true, arda: true }, // adim 1-4
  // { id: "13", variant: "E", load: "yogun", trace: true, arda: true }, // adim 1-5
  { id: "14", variant: "E", load: "yogun", trace: true, arda: true }, // adim 1-6
];
const VARIANT_TEXT = {
  A: "kapasite 4 · bekleme 0 tick",
  B: "kapasite 16 · bekleme 0 tick",
  C: "kapasite 4 · bekleme 20 ms",
  // D: "1 ms çerçeve · 32 slotluk port, 1 bayt tutamaç · 1 kurye · servis 20 ms", // kapali (adim 1)
  E: "adım 1–6 · 15 bayt iletim · port katmanı · 2 hat · seferde ≤ 3 · çerçeve fazı 500 µs",
};
// Kartta surum kodu 'Q' argumaninin 0..2 bitleri: A=0, B=1, C=2, Arda=3 ('D'), Arda2=4 ('E').
const VARIANT_CODES = "ABCDE";
const VARIANT_NAME = { D: "Arda", E: "Arda2" };
const vName = (v) => VARIANT_NAME[v] ?? v;
const isArda = (v) => v === "D" || v === "E";
// Arda'yi ('Q' 0x13) ve Arda2'yi ('Q' 0x14) taniyan firmware: arda.c ile
// derlenmis surum. Eski firmware bu komutlara yanit vermez (QRUN gelmedi hatasi).
const ARDA_FIRMWARE_READY = true;
const LOAD_TEXT = { normal: "normal", yogun: "yoğun" };

let port = null;
let reader = null;
let keepReading = false;
let rxBuffer = new Uint8Array(0);
let skipPartialLine = true; // baglanti anindaki yarim satir hata sayilmaz
const decoder = new TextDecoder("ascii");

let connectMs = 0;
let rawLog = []; // ham seri oturumu: "    12.345 < satir" (queue_experiment.py ile ayni)
let lineErrorCount = 0;
let ignoredLines = 0; // Hafta 2 satirlari
let boardTrace = null; // ACK/QRUN'dan: true | false | null (bilinmiyor)
let lastAck = "";
let ackTimer = null;
let boardError = "";

/* Suren kosu: { rec, tekrar, phase: "starting" | "running" | "dumping", qrun,
 * rows, counters, rawStart, startedIso, timer, busyRetried, waitBusy } */
let pending = null;
let runs = []; // biten kosular (meta.json bicimi + rows, counters, raw, saved)
let selectedIdx = null;
let runStatus = { text: "", cls: "" };
let progressTimer = null;
let storageOk = true;
let hoverSeq = null;

const el = (id) => document.getElementById(id);
const num = (s) => (/^\d+$/.test(s) ? Number(s) : null);
const allValid = (...xs) => xs.every((x) => x !== null);
const nf = (v, d) => v.toLocaleString("tr-TR", { minimumFractionDigits: d, maximumFractionDigits: d });
const msText = (us, d = 2) => (us === null || us === undefined ? "—" : nf(us / 1000, d));
// send_call A/B'de birkac us, C'de ~20 ms: 1 ms altinda us ile gosterilir.
const durParts = (us) => (us < 1000 ? [nf(us, us < 10 ? 1 : 0), "µs"] : [nf(us / 1000, 2), "ms"]);
const durText = (us) => (us === null || us === undefined ? "—" : durParts(us).join(" "));
const durHtml = (us) => (us === null || us === undefined ? "—" : durParts(us).join(" <small>") + "</small>");
const recById = (id) => RECORDS.find((r) => r.id === id);

function isoLocal(d = new Date()) {
  const pad = (n) => String(n).padStart(2, "0");
  const off = -d.getTimezoneOffset();
  const a = Math.abs(off);
  return (
    `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}T` +
    `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}` +
    `${off >= 0 ? "+" : "-"}${pad(Math.floor(a / 60))}:${pad(a % 60)}`
  );
}

/* ---------------------------------------------------------------------
 * Web Serial baglanti yonetimi
 * ------------------------------------------------------------------- */

async function connect() {
  if (!("serial" in navigator)) {
    el("serial-support-warning").hidden = false;
    return;
  }
  try {
    port = await navigator.serial.requestPort();
    // Varsayilan tampon 255 bayt: kosu sonundaki 56 satirlik doku kaybolmasin.
    await port.open({ baudRate: BAUD_RATE, dataBits: 8, stopBits: 1, parity: "none", bufferSize: 65536 });
  } catch (err) {
    console.error("Bağlantı hatası:", err);
    port = null;
    return;
  }
  keepReading = true;
  rxBuffer = new Uint8Array(0);
  skipPartialLine = true;
  connectMs = performance.now();
  rawLog = [];
  boardTrace = null;
  boardError = "";
  readLoop();
  render();
  queryBoard();
}

async function disconnect() {
  keepReading = false;
  try {
    if (reader) await reader.cancel();
  } catch (err) {
    /* zaten kapanmis olabilir */
  }
  try {
    if (port) await port.close();
  } catch (err) {
    /* yoksay */
  }
  port = null;
  reader = null;
  clearTimeout(ackTimer);
  boardTrace = null;
  boardError = "";
  if (pending) failRun("Bağlantı kesildi; koşu yarım kaldı.");
  render();
}

async function readLoop() {
  while (port && port.readable && keepReading) {
    reader = port.readable.getReader();
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        if (value && value.length) appendBytes(value);
      }
    } catch (err) {
      console.error("Okuma hatası:", err);
    } finally {
      reader.releaseLock();
    }
  }
}

function logRaw(direction, text) {
  rawLog.push(`${((performance.now() - connectMs) / 1000).toFixed(3).padStart(10)} ${direction} ${text}`);
  // Kosu yokken (or. telemetri acik kalmissa) oturum sinirsiz buyumesin.
  if (!pending && rawLog.length > 50000) rawLog.splice(0, rawLog.length - 20000);
}

const hex = (bytes) => Array.from(bytes, (b) => b.toString(16).toUpperCase().padStart(2, "0")).join(" ");

async function sendCommand(type, arg) {
  if (!port || !port.writable) return false;
  const b = new Uint8Array([CMD_SYNC0, CMD_SYNC1, type, arg, 0]);
  b[4] = (b[0] + b[1] + b[2] + b[3]) & 0xff;
  const writer = port.writable.getWriter();
  try {
    await writer.write(b);
    logRaw(">", hex(b));
    return true;
  } catch (err) {
    console.error("Komut gönderilemedi:", err);
    return false;
  } finally {
    writer.releaseLock();
  }
}

// Durum sorgusu: kart yalnizca ACK yazar (izleme bayragi buradan okunur).
async function queryBoard() {
  if (!(await sendCommand(CMD_TYPE_SCENARIO, CMD_ARG_QUERY))) return;
  clearTimeout(ackTimer);
  ackTimer = setTimeout(() => {
    boardError = "Kart durum sorgusuna yanıt vermedi. USB-TTL bağlantısı (TX → PA3, RX → PA2, GND) ve kartın çalıştığını kontrol edin.";
    renderBoard();
  }, ACK_TIMEOUT_MS);
}

/* ---------------------------------------------------------------------
 * Cerceve cozme: 64 baytlik ASCII satirlar ve (Hafta 2) kisa ikili cerceveler
 * ------------------------------------------------------------------- */

function appendBytes(chunk) {
  const merged = new Uint8Array(rxBuffer.length + chunk.length);
  merged.set(rxBuffer, 0);
  merged.set(chunk, rxBuffer.length);
  rxBuffer = merged;
  processBuffer();
}

function lineError() {
  lineErrorCount++;
  renderBoard();
}

// Gecerli bir cerceve gelene kadar arka arkaya gelen bozuk baytlar tek hata
// sayilir (baglanti anindaki yarim satir hic sayilmaz: skipPartialLine).
function frameError(bytes) {
  logRaw("<", "JUNK " + hex(bytes));
  if (!skipPartialLine) lineError();
  skipPartialLine = true;
}

const crc8 = (bytes) => {
  let crc = 0;
  for (const b of bytes) {
    crc ^= b;
    for (let i = 0; i < 8; i++) crc = crc & 0x80 ? ((crc << 1) ^ 0x07) & 0xff : (crc << 1) & 0xff;
  }
  return crc;
};

function processBuffer() {
  for (;;) {
    if (rxBuffer.length === 0) return;

    if (rxBuffer[0] === BIN_SYNC) {
      if (rxBuffer.length < 2) return;
      const size = BIN_SIZE[rxBuffer[1]];
      if (size !== undefined && rxBuffer.length < size) return; // cercevenin devami bekleniyor
      const frame = size === undefined ? null : rxBuffer.subarray(0, size);
      if (frame === null || crc8(frame.subarray(0, size - 1)) !== frame[size - 1]) {
        frameError(rxBuffer.subarray(0, 1)); // gecersiz tur ya da CRC: bir bayt atlanir
        rxBuffer = rxBuffer.slice(1);
        continue;
      }
      logRaw("<", "BIN " + hex(frame)); // Hafta 2 TEL/BTN: yok sayilir
      rxBuffer = rxBuffer.slice(size);
      skipPartialLine = false;
      ignoredLines++;
      continue;
    }

    // ASCII satirlarda 0xA5 hic gecmez: LF'den once gelen bir senkron bayti,
    // oncesinin yarim kalmis bir satir oldugunu gosterir.
    const lf = rxBuffer.indexOf(LF);
    const sync = rxBuffer.indexOf(BIN_SYNC);
    if (sync !== -1 && (lf === -1 || sync < lf)) {
      frameError(rxBuffer.subarray(0, sync));
      rxBuffer = rxBuffer.slice(sync);
      continue;
    }
    if (lf === -1) {
      if (rxBuffer.length > LINE_SIZE) {
        frameError(rxBuffer); // 64 bayt icinde LF yok: bozuk veri
        rxBuffer = new Uint8Array(0);
      }
      return;
    }
    let line = rxBuffer.subarray(0, lf + 1);
    rxBuffer = rxBuffer.slice(lf + 1);
    let recovered = false;
    if (line.length !== LINE_SIZE) {
      if (line.length < LINE_SIZE) {
        frameError(line);
        continue;
      }
      // Yarim satir arkasindaki tam satirla birlesmis: son 64 bayt tam satirdir.
      frameError(line.subarray(0, line.length - LINE_SIZE));
      line = line.subarray(line.length - LINE_SIZE);
      recovered = true;
    }
    skipPartialLine = false;
    const text = decoder.decode(line.subarray(0, LINE_SIZE - 1)).trimEnd();
    logRaw("<", text);
    if (!handleLine(text) && !recovered) lineError();
  }
}

function handleLine(text) {
  const f = text.split(",");
  switch (f[0]) {
    case "QRUN": return f.length === 9 && onQrun(f, text);
    case "QBUSY": return f.length === 2 && onQbusy(f);
    case "QMSG": return f.length === 9 && onQmsg(f);
    case "QCNT": return f.length === 4 && onQcnt(f);
    case "QEND": return f.length === 3 && onQend(f);
    case "ACK": return f.length === 4 && onAck(f, text);
    case "TEL": case "BTN": case "REC": case "CNT": case "END":
      ignoredLines++;
      return true;
    default:
      return false;
  }
}

/* ---------------------------------------------------------------------
 * Kosu yasami
 * ------------------------------------------------------------------- */

function setRunStatus(text, cls = "") {
  runStatus = { text, cls };
  renderRunStatus();
}

function armRunTimer(ms, message) {
  clearTimeout(pending.timer);
  pending.timer = setTimeout(() => failRun(message), ms);
}

function failRun(message) {
  if (pending) clearTimeout(pending.timer);
  pending = null;
  stopProgress();
  setRunStatus(message, "bad");
  render();
}

async function startRun(rec) {
  if (!port || pending) return;
  const done = runs.filter((r) => r.kosu.kayit === rec.id).length;
  if (done > 0 && !window.confirm(`Kayıt ${rec.id} bu tarayıcıda ${done} kez alındı. Tekrar (t${done + 1}) alınsın mı?`)) {
    return;
  }
  pending = {
    rec, tekrar: done + 1, phase: "starting", qrun: null, rows: [], counters: {}, rawStart: rawLog.length,
    startedIso: isoLocal(), timer: null, busyRetried: false, waitBusy: false,
  };
  setRunStatus(`Kayıt ${rec.id} (${vName(rec.variant)}/${LOAD_TEXT[rec.load]}) başlatılıyor…`, "busy");
  render();
  await sendRunCommand();
}

async function sendRunCommand() {
  if (!pending) return;
  const rec = pending.rec;
  const arg = VARIANT_CODES.indexOf(rec.variant) | (rec.load === "yogun" ? QRUN_LOAD_BURST : 0);
  if (!(await sendCommand(CMD_TYPE_QRUN, arg))) {
    failRun("Komut gönderilemedi.");
    return;
  }
  armRunTimer(QRUN_TIMEOUT_MS, "Kart koşu komutuna yanıt vermedi (QRUN gelmedi). Firmware Hafta 3 sürümü mü, USB-TTL TX → PA3 bağlı mı?");
}

function onAck(f, text) {
  const v = f[3];
  if (num(f[1]) === null || !/^S[0-6]$/.test(f[2]) || !/^[ABC]F?T?K?$/.test(v)) return false;
  clearTimeout(ackTimer);
  lastAck = text;
  boardTrace = v.includes("T");
  boardError = "";
  // Kosu baslamisken ACK yalnizca kart yeniden baslarsa gelir (acilis ACK'i).
  if (pending && pending.phase !== "starting") failRun("Kart koşu sırasında yeniden başladı (ACK geldi). Koşuyu tekrarlayın.");
  render();
  return true;
}

function onQrun(f, text) {
  const run = num(f[1]), capacity = num(f[4]), waitTicks = num(f[5]), serviceMs = num(f[6]), gapMs = num(f[7]);
  const variant = f[2], load = f[3];
  if (!allValid(run, capacity, waitTicks, serviceMs, gapMs) || !/^[ABCDE]$/.test(variant) ||
      !(load === "normal" || load === "yogun") || !(f[8] === "T" || f[8] === "-")) {
    return false;
  }
  boardTrace = f[8] === "T";
  if (!pending || pending.phase !== "starting") {
    render(); // baska bir aracin (or. queue_experiment.py) baslattigi kosu
    return true;
  }
  if (variant !== pending.rec.variant || load !== pending.rec.load) {
    failRun(`Kart farklı bir koşu bildirdi: ${text}`);
    return true;
  }
  pending.qrun = { run, variant, load, capacity, waitTicks, serviceMs, gapMs, trace: boardTrace, line: text };
  pending.phase = "running";
  pending.runStartMs = performance.now();
  armRunTimer(RUN_TIMEOUT_MS, `Koşu ${RUN_TIMEOUT_MS / 1000} s içinde bitmedi (QEND gelmedi).`);
  startProgress();
  render();
  return true;
}

function onQbusy(f) {
  if (num(f[1]) === null) return false;
  if (pending && pending.phase === "starting") {
    if (pending.busyRetried) {
      failRun("Kart hâlâ meşgul (QBUSY). Biraz bekleyip tekrar deneyin.");
    } else {
      pending.busyRetried = true;
      pending.waitBusy = true; // onceki kosunun QEND'i gelince komut bir kez daha gonderilir
      armRunTimer(RUN_TIMEOUT_MS, "Kart önceki koşuyu bitirmedi (QEND gelmedi).");
      setRunStatus("Kart önceki koşuyu bitiriyor, bekleniyor…", "busy");
    }
  }
  return true;
}

function onQmsg(f) {
  const run = num(f[1]), seq = num(f[2]), t = num(f[4]), send = num(f[5]);
  const opt = (s) => (s === "" ? null : num(s) ?? NaN); // bos alan gecerli (alinmadi), bozuk alan NaN
  const g2r = opt(f[6]), svc = opt(f[7]), depth = opt(f[8]);
  if (!allValid(run, seq, t, send) || !(f[3] === "ok" || f[3] === "fail") || [g2r, svc, depth].some(Number.isNaN)) {
    return false;
  }
  if (!pending?.qrun || run !== pending.qrun.run) return true; // baska kosuya ait
  const burst = pending.rec.load === "yogun" && seq >= BURST_FIRST && seq <= BURST_LAST;
  pending.rows.push({
    seq,
    phase: burst ? "art_arda" : "normal",
    send_result: f[3] === "ok" ? "accepted" : "failed",
    t_gen_us: t,
    send_call_us: send,
    generation_to_receive_us: g2r,
    service_us: svc,
    queue_depth_after_send: depth,
  });
  pending.phase = "dumping";
  renderRunStatus();
  return true;
}

function onQcnt(f) {
  const run = num(f[1]), value = num(f[3]);
  if (!allValid(run, value) || !/^[a-z_]+$/.test(f[2])) return false;
  if (pending?.qrun && run === pending.qrun.run) pending.counters[f[2]] = value;
  return true;
}

function onQend(f) {
  const run = num(f[1]), records = num(f[2]);
  if (!allValid(run, records)) return false;
  if (pending?.waitBusy) {
    pending.waitBusy = false;
    setRunStatus(`Kayıt ${pending.rec.id} başlatılıyor…`, "busy");
    sendRunCommand();
  } else if (pending?.qrun && run === pending.qrun.run) {
    finishRun(records);
  }
  return true;
}

function finishRun(records) {
  const p = pending;
  clearTimeout(p.timer);
  pending = null;
  stopProgress();
  const q = p.qrun;
  if (q.trace !== p.rec.trace) {
    setRunStatus(
      `Kartın izleme durumu (${q.trace ? "açık" : "kapalı"}) kayıt ${p.rec.id} ile uyuşmuyor; koşu kaydedilmedi. ` +
        `Firmware'i USE_SYSVIEW ${p.rec.trace ? 1 : 0} ile derleyip yükleyin.`,
      "bad",
    );
    render();
    return;
  }

  const rows = p.rows.sort((a, b) => a.seq - b.seq);
  const kontroller = checksFor(rows, p.counters, records, p.rec.load, q.variant);
  const info = port?.getInfo?.() ?? {};
  const run = {
    kosu: {
      kayit: p.rec.id, tekrar: p.tekrar, variant: q.variant, load: q.load, trace: q.trace, board_run: q.run,
      capacity: q.capacity, send_wait_ticks: q.waitTicks, service_ms: q.serviceMs, gap_ms: q.gapMs,
      attempts: ATTEMPTS, art_arda: q.load === "yogun" ? `${BURST_FIRST}..${BURST_LAST}` : null,
      baslangic: p.startedIso,
    },
    seri: {
      port: info.usbVendorId !== undefined
        ? `Web Serial USB ${info.usbVendorId.toString(16).padStart(4, "0")}:${(info.usbProductId ?? 0).toString(16).padStart(4, "0")}`
        : "Web Serial",
      baud: BAUD_RATE, qrun: q.line, ack: lastAck || null,
    },
    yazilim: {
      kaynak: "interface/index.html (Web Serial)",
      not: "Commit ve yuklu firmware bu araçtan okunamaz; README'de belirtin.",
    },
    systemview: null,
    kontroller,
    ozet: summarize(rows, p.counters),
    rows,
    counters: p.counters,
    raw: rawLog.slice(p.rawStart),
    saved: false,
  };
  runs.push(run);
  selectedIdx = runs.length - 1;
  persist();

  const bad = kontroller.filter((k) => !k.tamam).length;
  const o = run.ozet;
  setRunStatus(
    `Kayıt ${p.rec.id}${p.tekrar > 1 ? ` (t${p.tekrar})` : ""} bitti: ${o.accepted}/${o.attempted} kabul, ${o.failed} kayıp` +
      (bad ? ` · ${bad} kontrol başarısız.` : ". Dosyaları kaydetmeyi unutmayın."),
    bad ? "bad" : "good",
  );
  render();
}

/* ---------------------------------------------------------------------
 * Dogrulama ve ozet (analysis/queue_experiment.py checks_for / summarize)
 * ------------------------------------------------------------------- */

function checksFor(rows, c, end, load, variant) {
  const acc = rows.filter((r) => r.send_result === "accepted");
  const fail = rows.filter((r) => r.send_result === "failed");
  const seqs = rows.map((r) => r.seq).sort((a, b) => a - b);
  const chk = (kontrol, tamam, ayrinti = "") => ({ kontrol, tamam: !!tamam, ayrinti });
  const out = [
    chk("44 QMSG, seq 1..44", seqs.length === ATTEMPTS && seqs.every((s, i) => s === i + 1), `${rows.length} satır`),
    chk("QEND = QMSG sayısı", end === rows.length, `QEND ${end}`),
    chk("attempted = 44", c.attempted === ATTEMPTS, `${c.attempted}`),
    chk("attempted = accepted + failed", c.attempted !== undefined && c.attempted === c.accepted + c.failed,
      `${c.attempted} = ${c.accepted} + ${c.failed}`),
    chk("accepted = consumed (kuyruk boşaldı)", c.accepted !== undefined && c.accepted === c.consumed,
      `${c.accepted} / ${c.consumed}`),
    chk("QMSG ok/fail = sayaçlar", acc.length === c.accepted && fail.length === c.failed,
      `ok ${acc.length}, fail ${fail.length}`),
    chk("kabul edilenin gecikmesi var", acc.every((r) => r.generation_to_receive_us !== null)),
    chk("başarısızın gecikmesi boş (0 değil)",
      fail.every((r) => r.generation_to_receive_us === null && r.service_us === null)),
    chk("sıra hatası yok", c.order_error === 0, `${c.order_error}`),
    chk("tüketici koşu ortasında zaman aşımına düşmedi", c.rx_idle_timeout === 0, `${c.rx_idle_timeout}`),
  ];
  if (load === "normal") out.push(chk("normal yükte kayıp yok", fail.length === 0, `${fail.length} kayıp`));
  if (isArda(variant)) { // Arda/Arda2: tasarim varsayimlari (firmware arda.h, port_layer.h)
    const lostSvc = acc.filter((r) => r.generation_to_receive_us === null).length; // kabul edildi, servis edilmedi
    out.push(chk("Arda: hiç kayıp yok (gönderimde ve serviste)", fail.length === 0 && lostSvc === 0,
      `gönderimde ${fail.length}, serviste ${lostSvc}`));
    out.push(chk("Arda: port dolmadı, tutamaç hatası yok",
      c.pool_empty === 0 && c.pool_alloc_error === 0 && c.pool_free_error === 0,
      `dolu ${c.pool_empty}, hata ${c.pool_alloc_error}/${c.pool_free_error}`));
    out.push(chk("Arda: her tutamaç geri döndü", c.pool_leak === 0, `${c.pool_leak}`));
    out.push(chk("Arda: döküm isteği beklemeden kondu", c.dump_retry === 0, `${c.dump_retry}`));
    out.push(chk("Arda: çerçeve kaçırılmadı", c.frame_miss === 0, `${c.frame_miss}`));
    const starts = [...acc].sort((a, b) => a.seq - b.seq).filter((r) => r.generation_to_receive_us !== null)
      .map((r) => r.t_gen_us + r.generation_to_receive_us);
    const inv = starts.filter((s, i) => i > 0 && s < starts[i - 1]).length;
    out.push(chk("Arda: servis başları sıra numarasını izledi (KAN-03)", inv === 0, `${inv} ters`));
    const late = lateSeqs(rows);
    out.push(chk(`Arda: deadline (${DEADLINE_MS} ms) kaçmadı (DL-04)`, late.length === 0,
      `${late.length} mesaj${late.length ? `: ${seqRanges(late)}` : ""}`));
    if (c.deadline_miss !== undefined) { // port katmanli firmware (kayit 10, 11)
      out.push(chk("Arda: deadline sayacı kayıtlarla tutarlı", c.deadline_miss === late.length,
        `sayaç ${c.deadline_miss}, kayıt ${late.length}`));
      out.push(chk("Arda: durum portu bayat okunmadı", c.status_stale === 0, `${c.status_stale}`));
      out.push(chk("Arda: durum portu her girişimden önce okundu", c.status_reads === ATTEMPTS, `${c.status_reads}`));
      // Adim 4 firmware'i: kartin kurye sayisi kayda uymali (eski firmware'de sayac yok).
      out.push(chk(`Arda: ${ARDA_COURIERS} kurye (hat), art arda bölümde hepsi yolda`,
        c.couriers === ARDA_COURIERS && c.courier_busy_max === ARDA_COURIERS,
        `kurye ${c.couriers}, aynı anda en çok ${c.courier_busy_max}`));
      // Adim 5 firmware'i: toplu servis siniri kayda uymali.
      out.push(chk(`Arda: seferde en çok ${ARDA_BATCH} mesaj, art arda bölümde dolu`,
        c.batch === ARDA_BATCH && c.batch_max === ARDA_BATCH, `sınır ${c.batch}, ölçülen ${c.batch_max}`));
      // Adim 6 firmware'i: cerceve tick kenarindan ARDA_FRAME_PHASE_US sonra baslamali.
      out.push(chk(`Arda: çerçeve fazı ${ARDA_FRAME_PHASE_US} µs (tick kenarından), sapma ≤ ${FRAME_PHASE_TOL_US} µs`,
        c.frame_phase_us === ARDA_FRAME_PHASE_US && c.frame_phase_min_us >= ARDA_FRAME_PHASE_US &&
        c.frame_phase_max_us <= ARDA_FRAME_PHASE_US + FRAME_PHASE_TOL_US,
        `ayar ${c.frame_phase_us}, ölçülen ${c.frame_phase_min_us}…${c.frame_phase_max_us} µs`));
    }
  }
  return out;
}

// Servisi deadline'dan gec baslayan (g2r > DEADLINE_US) kabul edilmis mesajlar.
const isLate = (r) => r.send_result === "accepted" && r.generation_to_receive_us !== null &&
  r.generation_to_receive_us > DEADLINE_US;
const lateSeqs = (rows) => rows.filter(isLate).map((r) => r.seq);
// [12, 13, 14, 20] -> "12–14, 20"
function seqRanges(seqs) {
  const out = [];
  for (const s of [...seqs].sort((a, b) => a - b)) {
    const last = out[out.length - 1];
    if (last && last[1] === s - 1) last[1] = s;
    else out.push([s, s]);
  }
  return out.map(([a, b]) => (a === b ? `${a}` : `${a}–${b}`)).join(", ");
}

const mean = (xs) => (xs.length ? xs.reduce((a, b) => a + b, 0) / xs.length : null);
const maxOrNull = (xs) => (xs.length ? Math.max(...xs) : null);
// En yakin sira yontemi (nearest-rank): gozlenen bir degerdir.
const p95 = (xs) => {
  if (!xs.length) return null;
  const s = [...xs].sort((a, b) => a - b);
  return s[Math.max(0, Math.ceil(0.95 * s.length) - 1)];
};

function summarize(rows, c) {
  const acc = rows.filter((r) => r.send_result === "accepted");
  const fail = rows.filter((r) => r.send_result === "failed");
  const g2r = acc.map((r) => r.generation_to_receive_us).filter((v) => v !== null);
  const svc = acc.map((r) => r.service_us).filter((v) => v !== null);
  const burst = rows.filter((r) => r.phase === "art_arda");
  return {
    attempted: c.attempted ?? null, accepted: c.accepted ?? null, failed: c.failed ?? null,
    consumed: c.consumed ?? null, queue_max: c.queue_max ?? null,
    art_arda_accepted: burst.filter((r) => r.send_result === "accepted").length,
    art_arda_failed: burst.filter((r) => r.send_result === "failed").length,
    send_ok_mean_us: mean(acc.map((r) => r.send_call_us)),
    send_ok_max_us: maxOrNull(acc.map((r) => r.send_call_us)),
    send_fail_mean_us: mean(fail.map((r) => r.send_call_us)),
    send_fail_max_us: maxOrNull(fail.map((r) => r.send_call_us)),
    producer_send_total_ms: rows.reduce((s, r) => s + r.send_call_us, 0) / 1000,
    g2r_mean_us: mean(g2r), g2r_p95_us: p95(g2r), g2r_max_us: maxOrNull(g2r),
    deadline_miss: lateSeqs(rows).length,
    service_mean_us: mean(svc),
    run_ms: c.run_us !== undefined ? c.run_us / 1000 : null,
    stack_hwm_producer: c.stack_hwm_producer ?? null,
    stack_hwm_consumer: c.stack_hwm_consumer ?? null,
  };
}

/* ---------------------------------------------------------------------
 * Disa aktarma (queue_experiment.py ile ayni dosya adlari ve bicimler)
 * ------------------------------------------------------------------- */

const MSG_FIELDS = [
  "kayit", "tekrar", "board_run", "variant", "load", "trace", "seq", "phase", "send_result",
  "t_gen_us", "send_call_us", "generation_to_receive_us", "service_us", "queue_depth_after_send",
];
const SUMMARY_FIELDS = [
  "kayit", "tekrar", "variant", "load", "trace", "capacity", "send_wait_ticks", "attempted",
  "accepted", "failed", "consumed", "queue_max", "art_arda_accepted", "art_arda_failed",
  "send_ok_mean_us", "send_ok_max_us", "send_fail_mean_us", "send_fail_max_us",
  "producer_send_total_ms", "g2r_mean_us", "g2r_p95_us", "g2r_max_us", "deadline_miss", "service_mean_us",
  "run_ms", "kontroller_tamam", "baslangic", "dosya",
];

function baseName(run) {
  const k = run.kosu;
  return `kayit${k.kayit}_${vName(k.variant)}_${k.load}_iz_${k.trace ? "acik" : "kapali"}` + (k.tekrar > 1 ? `_t${k.tekrar}` : "");
}

function download(name, text, type = "text/csv") {
  const blob = new Blob([text], { type });
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = name;
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

const csvCell = (v) => (v === null || v === undefined ? "" : String(v));

function saveRun(run) {
  const base = baseName(run);
  const k = run.kosu;
  const lines = [MSG_FIELDS.join(",")];
  for (const r of run.rows) {
    const row = { kayit: k.kayit, tekrar: k.tekrar, board_run: k.board_run, variant: k.variant, load: k.load,
      trace: k.trace ? "acik" : "kapali", ...r };
    lines.push(MSG_FIELDS.map((f) => csvCell(row[f])).join(","));
  }
  download(`${base}.csv`, lines.join("\r\n") + "\r\n");

  const counters = ["name,value", ...Object.entries(run.counters).map(([n, v]) => `${n},${v}`)];
  download(`${base}_counters.csv`, counters.join("\r\n") + "\r\n");
  download(`${base}_raw.txt`, run.raw.join("\n") + "\n", "text/plain");

  const { kosu, seri, yazilim, systemview, kontroller, ozet } = run;
  const meta = { kosu, seri, yazilim, systemview, kontroller, ozet };
  download(`${base}_meta.json`, JSON.stringify(meta, null, 2) + "\n", "application/json");

  run.saved = true;
  persist();
  render();
}

function downloadSummary() {
  const rows = [...runs].sort((a, b) => a.kosu.kayit.localeCompare(b.kosu.kayit) || a.kosu.tekrar - b.kosu.tekrar);
  const round1 = (v) => (typeof v === "number" && !Number.isInteger(v) ? Math.round(v * 10) / 10 : v);
  const lines = [SUMMARY_FIELDS.join(",")];
  for (const run of rows) {
    const k = run.kosu;
    const row = {
      kayit: k.kayit, tekrar: k.tekrar, variant: k.variant, load: k.load, trace: k.trace ? "acik" : "kapali",
      capacity: k.capacity, send_wait_ticks: k.send_wait_ticks,
      kontroller_tamam: run.kontroller.every((x) => x.tamam) ? "True" : "False",
      baslangic: k.baslangic, dosya: baseName(run),
    };
    for (const [key, v] of Object.entries(run.ozet)) if (SUMMARY_FIELDS.includes(key)) row[key] = round1(v);
    lines.push(SUMMARY_FIELDS.map((f) => csvCell(row[f])).join(","));
  }
  download("queue_summary_tarayici.csv", lines.join("\r\n") + "\r\n");
}

/* ---------------------------------------------------------------------
 * Tarayicida saklama: sayfa yenilense de biten kosular kaybolmaz.
 * ------------------------------------------------------------------- */

function persist() {
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify({ runs }));
    storageOk = true;
  } catch (err) {
    storageOk = false; // gizli pencere ya da kota: yalnizca bu sayfa acikken tutulur
  }
}

function restore() {
  try {
    const data = JSON.parse(localStorage.getItem(STORAGE_KEY) || "null");
    if (data && Array.isArray(data.runs)) runs = data.runs.filter((r) => r && r.kosu && Array.isArray(r.rows));
  } catch (err) {
    runs = [];
  }
  selectedIdx = runs.length ? runs.length - 1 : null;
}

function clearSession() {
  const unsaved = runs.filter((r) => !r.saved).length;
  const msg = unsaved
    ? `${unsaved} koşu kaydedilmedi. Tarayıcıdaki ${runs.length} koşunun hepsi silinsin mi?`
    : `Tarayıcıdaki ${runs.length} koşu silinsin mi? (İndirilen dosyalar etkilenmez.)`;
  if (!window.confirm(msg)) return;
  runs = [];
  selectedIdx = null;
  persist();
  setRunStatus("");
  render();
}

/* ---------------------------------------------------------------------
 * Ilerleme
 * ------------------------------------------------------------------- */

function startProgress() {
  stopProgress();
  progressTimer = setInterval(renderRunStatus, 100);
}

function stopProgress() {
  clearInterval(progressTimer);
  progressTimer = null;
}

/* ---------------------------------------------------------------------
 * Gorunum
 * ------------------------------------------------------------------- */

function render() {
  renderConn();
  renderBoard();
  renderMatrix();
  renderRunStatus();
  renderResult();
  renderCompare();
}

function renderConn() {
  const connected = !!port;
  const status = el("conn-status");
  status.textContent = connected ? "Bağlı" : "Bağlı değil";
  status.className = "status " + (connected ? "status--connected" : "status--disconnected");
  el("btn-connect").disabled = connected || !("serial" in navigator);
  el("btn-disconnect").disabled = !connected;
}

function renderBoard() {
  const box = el("board-status");
  let text;
  if (!port) text = "Bağlı değil";
  else if (boardError) text = boardError;
  else if (boardTrace === null) text = "Kartın durumu bekleniyor…";
  else text = `Kart hazır · SystemView izlemesi ${boardTrace ? "açık: kayıt 01–06, 14" : "kapalı: kayıt 07"}`;
  if (port && (lineErrorCount || ignoredLines)) {
    text += ` · hatalı satır ${lineErrorCount}` + (ignoredLines ? ` · Hafta 2 satırı ${ignoredLines} (yok sayıldı)` : "");
  }
  box.textContent = text;
  box.classList.toggle("bad", !!boardError);
}

function recState(rec) {
  const done = runs.filter((r) => r.kosu.kayit === rec.id);
  if (pending?.rec.id === rec.id) return { text: "çalışıyor…", cls: "" };
  if (!done.length) return { text: "alınmadı", cls: "" };
  const bad = done.some((r) => !r.kontroller.every((x) => x.tamam));
  const unsaved = done.filter((r) => !r.saved).length;
  return {
    text: (bad ? "✗ " : "✓ ") + `${done.length} koşu` + (unsaved ? ` · ${unsaved} kaydedilmedi` : " · kaydedildi"),
    cls: bad ? "bad" : "done",
  };
}

function recButton(rec, selected) {
  const notReady = rec.arda && !ARDA_FIRMWARE_READY;
  const mismatch = boardTrace !== null && boardTrace !== rec.trace;
  const disabled = !port || !!pending || mismatch || notReady;
  const st = notReady
    ? { text: "firmware'de henüz yok · tasarım aşamasında", cls: "" }
    : mismatch
      ? { text: rec.trace ? "iz açık firmware gerekir" : "iz kapalı firmware gerekir", cls: "" }
      : recState(rec);
  const title = notReady
    ? "Kart bu koşuyu henüz tanımıyor. Firmware'e Arda sürümleri eklenince açılır."
    : mismatch
      ? `Kart izleme ${boardTrace ? "açık" : "kapalı"} derlenmiş; bu kayıt için USE_SYSVIEW ${rec.trace ? 1 : 0} ile derleyip yükleyin.`
      : `Kayıt ${rec.id}: ${vName(rec.variant)}/${LOAD_TEXT[rec.load]} koşusunu başlat`;
  const cls = ["rec-btn"];
  if (rec.arda) cls.push("rec-btn--arda");
  if (pending?.rec.id === rec.id) cls.push("running");
  if (selected && selected.kosu.kayit === rec.id) cls.push("selected");
  return `<button class="${cls.join(" ")}" data-rec="${rec.id}" title="${title}"${disabled ? " disabled" : ""}>
      <b>${rec.id} · ${vName(rec.variant)} / ${LOAD_TEXT[rec.load]}</b>
      <span>${VARIANT_TEXT[rec.variant]}</span>
      <span>iz ${rec.trace ? "açık" : "kapalı"}</span>
      <em class="${st.cls}">${st.text}</em>
    </button>`;
}

function renderMatrix() {
  const selected = selectedRun();
  el("rec-grid").innerHTML = RECORDS.filter((r) => !r.arda).map((r) => recButton(r, selected)).join("");
  el("rec-grid-arda").innerHTML = RECORDS.filter((r) => r.arda).map((r) => recButton(r, selected)).join("");
}

function renderRunStatus() {
  const bar = el("run-progress-bar");
  const box = el("run-progress");
  const status = el("run-status");
  if (pending && pending.phase !== "starting") {
    const p = pending;
    const tag = `Kayıt ${p.rec.id} · ${vName(p.rec.variant)}/${LOAD_TEXT[p.rec.load]} · kart koşusu ${p.qrun.run}`;
    box.hidden = false;
    if (p.phase === "running") {
      const elapsed = performance.now() - p.runStartMs;
      const expected = EXPECTED_RUN_MS[p.rec.load];
      bar.style.width = `${Math.min(97, (elapsed / expected) * 100)}%`;
      bar.style.background = "var(--warn)";
      status.textContent = `${tag} sürüyor: ${nf(elapsed / 1000, 1)} s / ~${nf(expected / 1000, 1)} s`;
    } else {
      bar.style.width = `${(p.rows.length / ATTEMPTS) * 100}%`;
      bar.style.background = "var(--accent)";
      status.textContent = `${tag} bitti, kayıtlar alınıyor: ${p.rows.length} / ${ATTEMPTS}`;
    }
    status.className = "run-status busy";
    return;
  }
  box.hidden = true;
  status.textContent = runStatus.text;
  status.className = "run-status" + (runStatus.cls ? " " + runStatus.cls : "");
}

const selectedRun = () => (selectedIdx !== null && runs[selectedIdx]) || null;
const runLabel = (r) =>
  `Kayıt ${r.kosu.kayit}${r.kosu.tekrar > 1 ? ` t${r.kosu.tekrar}` : ""} · ${vName(r.kosu.variant)}/${LOAD_TEXT[r.kosu.load]} · iz ${r.kosu.trace ? "açık" : "kapalı"}`;

// Arda: cerceve, port ve deadline sayaclari (firmware arda.h, port_layer.h; dokumde QCNT).
function ardaTiles(c, rows) {
  const v = (name) => (c[name] === undefined ? null : c[name]);
  const dtMin = v("frame_dt_min_us"), dtMax = v("frame_dt_max_us");
  const jitter = dtMin === null || dtMax === null ? null : Math.max(1000 - dtMin, dtMax - 1000);
  return [
    tile("Arda çerçevesi", `${v("frames") ?? "—"}`, `kaçırılan ${v("frame_miss") ?? "—"}`, v("frame_miss") === 0 ? "good" : "bad"),
    tile("Çerçeve aralığı", jitter === null ? "—" : `±${durText(jitter)}`,
      `${durText(dtMin)} … ${durText(dtMax)} (ideal 1 ms)`),
    tile("Çerçeve işi (en uzun)", durHtml(v("frame_work_max_us")), "boşaltma + sefer, 1 ms bütçeden"),
    tile("Porttan alınma (en geç)", durHtml(v("intake_max_us")), "üretimden mesaj portundan alınmaya"),
    tile("Gelen kutusu (tepe)", `${v("inbox_max") ?? "—"} <small>/ 32</small>`, "servisini bekleyen en çok mesaj"),
    (() => {
      const errs = ["pool_empty", "pool_alloc_error", "pool_free_error", "pool_leak"].map(v);
      const ok = errs.every((x) => x === 0);
      return tile("Mesaj portu (queuing)", ok ? "temiz" : "hata",
        `dolu ${errs[0] ?? "—"} · hata ${errs[1] ?? "—"}/${errs[2] ?? "—"} · sızıntı ${errs[3] ?? "—"}`, ok ? "good" : "bad");
    })(),
    (() => {
      const late = lateSeqs(rows);
      const counter = v("deadline_miss");
      return tile(`Deadline (${DEADLINE_MS} ms) kaçan`, `${late.length}`,
        (late.length ? `mesaj ${seqRanges(late)}` : "hepsi zamanında") +
        (counter === null ? "" : ` · kart sayacı ${counter}`), late.length === 0 ? "good" : "bad");
    })(),
    ...(v("status_reads") === null ? [] : [
      tile("Durum portu (sampling)", v("status_stale") === 0 ? "taze" : `bayat ${v("status_stale")}`,
        `yazma ${v("status_writes")} · okuma ${v("status_reads")} · okunmadan üstüne yazılan ${v("status_missed")}` +
        ` · yaş en çok ${durText(v("status_age_max_us"))}`, v("status_stale") === 0 ? "good" : "bad"),
      tile("Producer'ın gördüğü birikme", `${v("status_backlog_max")} <small>/ ${v("inbox_max") ?? "—"}</small>`,
        "durum portundaki en büyük değer / gelen kutusunun gerçek tepesi"),
    ]),
    ...(v("couriers") === null ? [] : [
      tile("Kurye (hat)", `${v("courier_busy_max")} <small>/ ${v("couriers")}</small>`,
        "aynı anda yolda olan en çok kurye / hat sayısı", v("courier_busy_max") === v("couriers") ? "good" : ""),
    ]),
    ...(v("batch") === null ? [] : [
      tile("Toplu sefer", `${v("batch_max")} <small>/ ${v("batch")}</small>`,
        "bir seferdeki en çok mesaj / sınır (arka arkaya, aynı hatta)", v("batch_max") === v("batch") ? "good" : ""),
    ]),
    ...(v("frame_phase_us") === null ? [] : [
      tile("Çerçeve fazı", `${v("frame_phase_min_us")}…${v("frame_phase_max_us")} <small>µs</small>`,
        `tick kenarından sonra · ayar ${v("frame_phase_us")} µs`,
        v("frame_phase_min_us") >= v("frame_phase_us") && v("frame_phase_max_us") <= v("frame_phase_us") + FRAME_PHASE_TOL_US
          ? "good" : "bad"),
    ]),
  ];
}

function tile(label, value, note = "", cls = "") {
  return `<div class="tile"><div class="tile-label">${label}</div>` +
    `<div class="tile-value${cls ? " " + cls : ""}">${value}</div>` +
    (note ? `<div class="tile-note">${note}</div>` : "") + "</div>";
}

function renderResult() {
  const sel = el("run-select");
  sel.innerHTML = runs.map((r, i) => `<option value="${i}">${runLabel(r)}</option>`).join("");
  sel.disabled = runs.length === 0;
  const run = selectedRun();
  el("btn-save-run").disabled = !run;
  el("result-empty").hidden = !!run;
  el("result-body").hidden = !run;
  if (!run) {
    el("run-saved").textContent = "";
    return;
  }
  sel.value = String(selectedIdx);
  el("run-saved").textContent = run.saved ? `Kaydedildi: ${baseName(run)}{.csv,…}` : `Kaydedilmedi · ${baseName(run)}`;
  el("run-saved").classList.toggle("bad", !run.saved);

  const o = run.ozet;
  const k = run.kosu;
  const burst = k.load === "yogun";
  el("tiles").innerHTML = [
    tile("Kabul edilen", `${o.accepted} <small>/ ${o.attempted}</small>`, `tüketilen ${o.consumed}`, o.failed === 0 ? "good" : ""),
    tile("Başarısız gönderim", String(o.failed), burst ? `art arda bölümde ${o.art_arda_failed} / ${BURST_LAST - BURST_FIRST + 1}` : "",
      o.failed > 0 ? "bad" : ""),
    tile("Kuyruk tepe doluluğu", `${o.queue_max} <small>/ ${k.capacity}</small>`,
      `bekleme ${k.send_wait_ticks} tick · servis ${k.service_ms} ms`),
    tile("Üretim → alma (ort.)", `${msText(o.g2r_mean_us)} <small>ms</small>`,
      `p95 ${msText(o.g2r_p95_us)} · maks ${msText(o.g2r_max_us)} ms`),
    tile("send_call, başarısız (ort.)", durHtml(o.send_fail_mean_us),
      `kabul ort. ${durText(o.send_ok_mean_us)} · üretici gönderimde toplam ${nf(o.producer_send_total_ms, 1)} ms`),
    tile("Koşu süresi", o.run_ms === null ? "—" : `${nf(o.run_ms / 1000, 2)} <small>s</small>`,
      `servis ort. ${msText(o.service_mean_us)} ms`),
    tile("Yığın (en az boş)", `${o.stack_hwm_producer ?? "—"} / ${o.stack_hwm_consumer ?? "—"}`,
      isArda(k.variant) ? "Producer / Arda çerçevesi, word" : "Producer / Consumer, word"),
    ...(isArda(k.variant) ? ardaTiles(run.counters, run.rows) : []),
  ].join("");

  el("checks").innerHTML = run.kontroller
    .map((c) => `<li class="${c.tamam ? "" : "bad"}" title="${c.ayrinti}">${c.kontrol}${c.tamam || !c.ayrinti ? "" : ` (${c.ayrinti})`}</li>`)
    .join("");

  el("msg-tbody").innerHTML = run.rows
    .map((r) => {
      const ok = r.send_result === "accepted";
      const cls = [ok ? "" : "fail", r.phase === "art_arda" ? "burst" : "",
        isArda(k.variant) && isLate(r) ? "late" : ""].filter(Boolean).join(" ");
      return `<tr${cls ? ` class="${cls}"` : ""}>
        <td>${r.seq}</td>
        <td>${r.phase === "art_arda" ? "art arda" : "normal"}</td>
        <td>${ok ? '<span class="badge badge--ok">kabul</span>' : '<span class="badge badge--miss">başarısız</span>'}</td>
        <td>${msText(r.t_gen_us, 1)}</td>
        <td>${durText(r.send_call_us)}</td>
        <td>${msText(r.generation_to_receive_us)}</td>
        <td>${msText(r.service_us)}</td>
        <td>${r.queue_depth_after_send === null ? "—" : `${r.queue_depth_after_send}/${k.capacity}`}</td>
      </tr>`;
    })
    .join("");

  drawTimeline();
}

function renderCompare() {
  const order = runs.map((r, i) => i).sort((a, b) =>
    runs[a].kosu.kayit.localeCompare(runs[b].kosu.kayit) || runs[a].kosu.tekrar - runs[b].kosu.tekrar);
  el("compare-tbody").innerHTML = order
    .map((i) => {
      const r = runs[i];
      const k = r.kosu;
      const o = r.ozet;
      const ok = r.kontroller.every((x) => x.tamam);
      return `<tr data-idx="${i}"${i === selectedIdx ? ' class="selected"' : ""}>
        <td>${k.kayit}${k.tekrar > 1 ? ` t${k.tekrar}` : ""}</td>
        <td>${vName(k.variant)} / ${LOAD_TEXT[k.load]}</td>
        <td>${k.trace ? "açık" : "kapalı"}</td>
        <td>${o.accepted}</td>
        <td>${o.failed}</td>
        <td>${k.load === "yogun" ? `${o.art_arda_accepted} / ${o.art_arda_failed}` : "—"}</td>
        <td>${o.queue_max}/${k.capacity}</td>
        <td>${durText(o.send_fail_mean_us)}</td>
        <td>${msText(o.g2r_mean_us)} ms</td>
        <td>${msText(o.g2r_p95_us)} ms</td>
        <td>${msText(o.g2r_max_us)} ms</td>
        <td>${o.run_ms === null ? "—" : nf(o.run_ms / 1000, 2) + " s"}</td>
        <td>${ok ? '<span class="badge badge--ok">tamam</span>' : '<span class="badge badge--miss">hata</span>'}</td>
        <td>${r.saved ? "kaydedildi" : '<span class="badge badge--pending">kaydedilmedi</span>'}</td>
      </tr>`;
    })
    .join("");
  el("btn-summary").disabled = runs.length === 0;
  el("btn-clear-session").disabled = runs.length === 0;
}

/* ---------------------------------------------------------------------
 * Zaman cizelgesi: her girisim bir satir. Cizim tamponu canvas'in gercek
 * CSS boyutu x devicePixelRatio'ya ayarlanir (gerilmis yazi olmasin).
 * ------------------------------------------------------------------- */

let tlLayout = null;

function niceStep(raw) {
  const p = 10 ** Math.floor(Math.log10(raw));
  for (const m of [1, 2, 5, 10]) if (m * p >= raw) return m * p;
  return 10 * p;
}

function rowEndUs(r) {
  if (r.send_result === "failed" || r.generation_to_receive_us === null) return r.t_gen_us + r.send_call_us;
  return r.t_gen_us + r.generation_to_receive_us + (r.service_us ?? 0);
}

function drawTimeline() {
  const run = selectedRun();
  const canvas = el("timeline");
  const w = canvas.clientWidth;
  const h = canvas.clientHeight;
  if (!run || w === 0 || h === 0) return;
  const dpr = window.devicePixelRatio || 1;
  if (canvas.width !== Math.round(w * dpr) || canvas.height !== Math.round(h * dpr)) {
    canvas.width = Math.round(w * dpr);
    canvas.height = Math.round(h * dpr);
  }
  const ctx = canvas.getContext("2d");
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);

  const css = getComputedStyle(document.documentElement);
  const color = (name) => css.getPropertyValue(name).trim();
  const padL = 46, padR = 18, padT = 10, padB = 30;
  const plotW = w - padL - padR;
  const plotH = h - padT - padB;
  const rowH = plotH / ATTEMPTS;
  const maxMs = Math.max(1, ...run.rows.map(rowEndUs)) / 1000;
  const stepMs = niceStep(maxMs / 7);
  const axisMs = Math.ceil(maxMs / stepMs) * stepMs;
  const xOf = (us) => padL + (us / 1000 / axisMs) * plotW;
  const yOf = (seq) => padT + (seq - 1) * rowH;
  tlLayout = { padL, padT, rowH, plotW, plotH, w };

  // Art arda bolum
  if (run.kosu.load === "yogun") {
    ctx.fillStyle = "rgba(255,176,32,0.07)";
    ctx.fillRect(padL, yOf(BURST_FIRST), plotW, rowH * (BURST_LAST - BURST_FIRST + 1));
    ctx.strokeStyle = "rgba(255,176,32,0.35)";
    ctx.lineWidth = 1;
    ctx.strokeRect(padL + 0.5, yOf(BURST_FIRST) + 0.5, plotW - 1, rowH * (BURST_LAST - BURST_FIRST + 1) - 1);
  }

  // Izgara ve eksenler
  ctx.font = "11px sans-serif";
  ctx.fillStyle = color("--text-dim");
  ctx.strokeStyle = "rgba(154,164,184,0.12)";
  ctx.textAlign = "center";
  ctx.textBaseline = "top";
  for (let t = 0; t <= axisMs + 1e-9; t += stepMs) {
    const x = padL + (t / axisMs) * plotW;
    ctx.beginPath();
    ctx.moveTo(x + 0.5, padT);
    ctx.lineTo(x + 0.5, padT + plotH);
    ctx.stroke();
    // Binlik ayirici yok: "2.500 ms" 2,5 ms gibi okunmasin.
    ctx.fillText(`${Math.round(t)} ms`, Math.min(x, w - padR - 18), padT + plotH + 8);
  }
  ctx.textAlign = "right";
  ctx.textBaseline = "middle";
  for (const s of [1, 10, 20, 30, 40, ATTEMPTS]) {
    ctx.fillText(String(s), padL - 8, yOf(s) + rowH / 2);
  }
  ctx.save();
  ctx.translate(12, padT + plotH / 2);
  ctx.rotate(-Math.PI / 2);
  ctx.textAlign = "center";
  ctx.fillText("seq", 0, 0);
  ctx.restore();

  if (hoverSeq !== null) {
    ctx.fillStyle = "rgba(76,154,255,0.12)";
    ctx.fillRect(padL, yOf(hoverSeq), plotW, rowH);
  }

  // Cubuklar: en az 2 px, kisa olaylar da gorunsun
  const barH = Math.max(2, rowH * 0.62);
  const span = (x0, x1, y, fill) => {
    ctx.fillStyle = fill;
    ctx.fillRect(x0, y, Math.max(2, x1 - x0), barH);
  };
  for (const r of run.rows) {
    const y = yOf(r.seq) + (rowH - barH) / 2;
    const x0 = xOf(r.t_gen_us);
    if (r.send_result === "failed") {
      span(x0, xOf(r.t_gen_us + r.send_call_us), y, color("--bad"));
      continue;
    }
    const sendEnd = r.t_gen_us + r.send_call_us;
    const recv = r.t_gen_us + (r.generation_to_receive_us ?? r.send_call_us);
    if (xOf(sendEnd) - x0 >= 1) span(x0, xOf(sendEnd), y, color("--block"));
    if (xOf(recv) - xOf(sendEnd) >= 1) span(xOf(sendEnd), xOf(recv), y, color("--warn"));
    if (r.service_us !== null) span(xOf(recv), xOf(recv + r.service_us), y, color("--accent"));
  }

  ctx.strokeStyle = color("--panel-border");
  ctx.beginPath();
  ctx.moveTo(padL + 0.5, padT);
  ctx.lineTo(padL + 0.5, padT + plotH);
  ctx.lineTo(padL + plotW, padT + plotH + 0.5);
  ctx.stroke();
}

function onTimelineMove(e) {
  const run = selectedRun();
  if (!run || !tlLayout) return;
  const rect = el("timeline").getBoundingClientRect();
  const x = e.clientX - rect.left;
  const y = e.clientY - rect.top;
  const seq = Math.floor((y - tlLayout.padT) / tlLayout.rowH) + 1;
  const tip = el("tl-tip");
  if (x < tlLayout.padL || seq < 1 || seq > ATTEMPTS) {
    onTimelineLeave();
    return;
  }
  if (seq !== hoverSeq) {
    hoverSeq = seq;
    drawTimeline();
  }
  const r = run.rows.find((row) => row.seq === seq);
  if (!r) return;
  const ok = r.send_result === "accepted";
  tip.innerHTML =
    `<b>seq ${seq}</b> <span class="dim">· ${r.phase === "art_arda" ? "art arda" : "normal"} · ` +
    `${ok ? "kabul" : "başarısız"}</span><br>` +
    `üretim: ${msText(r.t_gen_us, 1)} ms<br>` +
    `send_call: ${durText(r.send_call_us)}<br>` +
    (ok
      ? `üretim→alma: ${msText(r.generation_to_receive_us)} ms<br>servis: ${msText(r.service_us)} ms<br>` +
        `gönderimden sonra kuyrukta: ${r.queue_depth_after_send}/${run.kosu.capacity}`
      : `<span class="dim">kuyruğa giremedi: gecikme yok</span>`);
  tip.hidden = false;
  const tw = tip.offsetWidth;
  tip.style.left = `${Math.min(x + 14, rect.width - tw - 4)}px`;
  tip.style.top = `${Math.max(4, y - 10)}px`;
}

function onTimelineLeave() {
  el("tl-tip").hidden = true;
  if (hoverSeq !== null) {
    hoverSeq = null;
    drawTimeline();
  }
}

/* ---------------------------------------------------------------------
 * Kurulum
 * ------------------------------------------------------------------- */

if (!("serial" in navigator)) {
  el("serial-support-warning").hidden = false;
}

el("btn-connect").addEventListener("click", connect);
el("btn-disconnect").addEventListener("click", disconnect);
for (const id of ["rec-grid", "rec-grid-arda"]) {
  el(id).addEventListener("click", (e) => {
    const btn = e.target.closest(".rec-btn");
    if (btn && !btn.disabled) startRun(recById(btn.dataset.rec));
  });
}
el("run-select").addEventListener("change", (e) => {
  selectedIdx = Number(e.target.value);
  hoverSeq = null;
  render();
});
el("compare-tbody").addEventListener("click", (e) => {
  const row = e.target.closest("tr[data-idx]");
  if (!row) return;
  selectedIdx = Number(row.dataset.idx);
  hoverSeq = null;
  render();
});
el("btn-save-run").addEventListener("click", () => {
  const run = selectedRun();
  if (run) saveRun(run);
});
el("btn-summary").addEventListener("click", downloadSummary);
el("btn-clear-session").addEventListener("click", clearSession);
el("timeline").addEventListener("mousemove", onTimelineMove);
el("timeline").addEventListener("mouseleave", onTimelineLeave);
window.addEventListener("resize", drawTimeline);
window.addEventListener("beforeunload", (e) => {
  // Kosular tarayicida saklaniyorsa yenileme veri kaybettirmez; saklanamiyorsa uyar.
  if (pending || (!storageOk && runs.some((r) => !r.saved))) {
    e.preventDefault();
    e.returnValue = "";
  }
});

restore();
render();

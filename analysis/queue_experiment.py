#!/usr/bin/env python3
"""
queue_experiment.py - Hafta 3 queue deneyi kosulari (odev-03)

Karta (USART2, 230400 8N1, USB-TTL adaptor) kosu komutunu gonderir, kosu
bitince dokulen QMSG/QCNT/QEND satirlarini toplar, sayac esitliklerini
dogrular ve her kosu icin su dosyalari yazar:

  measurements/kayitNN_<surum>_<yuk>_iz_<acik|kapali>.csv           mesaj basina olcum
  measurements/kayitNN_<surum>_<yuk>_iz_<acik|kapali>_counters.csv  kart sayaclari
  measurements/kayitNN_<surum>_<yuk>_iz_<acik|kapali>_raw.txt       ham seri oturumu
  measurements/kayitNN_<surum>_<yuk>_iz_<acik|kapali>_meta.json     ayarlar, kontroller, ozet
  traces/kayitNN_<surum>_<yuk>.SVDat                                (--systemview ile)

Her kosudan sonra measurements/queue_summary.csv (karsilastirma tablosu) butun
meta dosyalarindan yeniden olusturulur.

Kayit matrisi (odev-03):
  01 A normal   02 B normal   03 C normal      iz acik
  04 A yogun    05 B yogun    06 C yogun       iz acik
  07 A yogun                                   iz KAPALI: firmware USE_SYSVIEW 0
                                               ile derlenip yuklenmis olmali

Kullanim (depo kokunden):
  python analysis/queue_experiment.py --portlar                  seri portlari listele
  python analysis/queue_experiment.py --port COM5                01..06
  python analysis/queue_experiment.py --port COM5 --systemview   01..06 + .SVDat (J-Link)
  python analysis/queue_experiment.py --port COM5 --kayit 07     iz kapali firmware ile
  python analysis/queue_experiment.py --port COM5 --kayit 04-06 --tekrar 3
  python analysis/queue_experiment.py --ozet                     yalnizca ozet tablosu

Kartin izleme durumu (ACK ve QRUN satirindaki T) kayit matrisiyle uyusmazsa
kosu baslatilmaz. Ayni ada sahip eski dosyalar silinmez, eski/ altina tasinir.
Tekrarlarda ilk kosu eksiz, sonrakiler _t2, _t3 ... adini alir.

Ayni kosular web arayuzunden (interface/index.html) de alinabilir; arayuzun
indirdigi dosyalar ayni bicimdedir. measurements/ altina kopyalayip --ozet ile
tabloya katin.
B1 butonuna kosu sirasinda basmayin: buton satiri hatti ve UartTxTask'i
(Consumer ile ayni oncelik) kullanir.
"""
from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import math
import re
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MEAS_DIR = ROOT / "measurements"
TRACE_DIR = ROOT / "traces"
SUMMARY_CSV = MEAS_DIR / "queue_summary.csv"
SYSVIEW_RECORD = ROOT / "analysis" / "sysview_record.py"
FIRMWARE_OUT = ROOT / "firmware/EWARM/Debug/Exe/frametrace.out"

BAUD = 230400  # firmware/Core/Inc/main.h UART_BAUDRATE
LINE_SIZE = 64  # protocol.h: 63 bayt metin + LF
BIN_SYNC = 0xA5  # protocol.h: kisa ikili TEL/BTN cercevesi
BIN_SIZE = {ord("T"): 19, ord("B"): 20}
CMD_SCENARIO, CMD_QRUN = ord("C"), ord("Q")
QRUN_LOAD_BURST = 0x10  # protocol.h CMD_QRUN_LOAD_BURST
VARIANTS = "ABCDE"  # 'Q' arg bit 0..2; E = Arda2 (ek deney, firmware arda.h); D kapali, eski kayitlar icin okunur
VARIANT_NAME = {"D": "Arda", "E": "Arda2"}  # dosya adlari ve ekran; kartta tek harf
ARDA_VARIANTS = ("D", "E")
ATTEMPTS = 44  # queue_exp.h QEXP_ATTEMPTS
DEADLINE_MS = 20  # arda.h ARDA_DEADLINE_MS (DL-04): uretimden servis baslangicina
DEADLINE_US = DEADLINE_MS * 1000
ARDA_COURIERS = 2  # arda.h ARDA_COURIERS (adim 4: kurye = hat)
ARDA_BATCH = 3  # arda.h ARDA_BATCH_MAX (adim 5: toplu servis)
ARDA_FRAME_PHASE_US = 500  # arda.h ARDA_FRAME_PHASE_US (adim 6: cerceve tick kenarindan bu kadar sonra)
FRAME_PHASE_TOL_US = 50  # olculen faz: ayar .. ayar + 50 us (kesme ve gorev gecis gecikmesi)
BURST = range(11, 35)  # queue_exp.h QEXP_BURST_FIRST..QEXP_BURST_LAST

RECORDS = {
    "01": ("A", "normal", True), "02": ("B", "normal", True), "03": ("C", "normal", True),
    "04": ("A", "yogun", True), "05": ("B", "yogun", True), "06": ("C", "yogun", True),
    "07": ("A", "yogun", False),
    # Arda/yogun: odev matrisinin disinda. Adimlar ust uste eklendi; kayit 14 altisinin toplamidir
    # (arda.h). Ara adimlarin kayitlari kapali:
    # "08": ("D", "yogun", True),  # adim 1: servis 20 ms
    # "09": ("E", "yogun", True),  # adim 1 + 2: servis = 15 baytlik iletim modeli
    # "10": ("D", "yogun", True),  # adim 1 + 3 (adim 2 olmadan)
    # "11": ("E", "yogun", True),  # adim 1 + 2 + 3: port katmani, deadline ve durum portu sayaclari
    # "12": ("E", "yogun", True),  # adim 1-4: ikinci kurye (hat)
    # "13": ("E", "yogun", True),  # adim 1-5: toplu servis, seferde en cok 3 mesaj
    "14": ("E", "yogun", True),  # adim 1-6: cerceve TIM2'den, tick kenarindan 500 us sonra
}


def vname(variant: str) -> str:
    return VARIANT_NAME.get(variant, variant)

ACK_TIMEOUT_S = 2.0
QRUN_TIMEOUT_S = 3.0
RUN_TIMEOUT_S = 20.0  # normal kosu ~4,4 s + doku ~0,2 s
SYSVIEW_SECONDS = 9.0  # kosu + doku + pay


class RunError(Exception):
    pass


# --------------------------------------------------------------------------
# Seri cerceve cozumu (interface/app.js ile ayni kurallar)
# --------------------------------------------------------------------------

def crc8(data: bytes) -> int:
    """protocol.c protocol_crc8: polinom 0x07, baslangic 0."""
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


class FrameReader:
    """Bayt akisini 64 baytlik ASCII satirlara ve kisa ikili cercevelere boler.
    Baglanti anindaki yarim satir ya da bozuk bayt 'junk' olarak atilir."""

    def __init__(self) -> None:
        self.buf = bytearray()

    def feed(self, data: bytes) -> list[tuple[str, object]]:
        self.buf += data
        out: list[tuple[str, object]] = []
        while self.buf:
            if self.buf[0] == BIN_SYNC:
                if len(self.buf) < 2:
                    break
                size = BIN_SIZE.get(self.buf[1])
                if size is not None and len(self.buf) < size:
                    break
                if size is not None and crc8(self.buf[:size - 1]) == self.buf[size - 1]:
                    out.append(("bin", bytes(self.buf[:size])))
                    del self.buf[:size]
                else:
                    out.append(("junk", bytes(self.buf[:1])))
                    del self.buf[:1]
                continue
            nl = self.buf.find(b"\n")
            sync = self.buf.find(BIN_SYNC)
            if sync >= 0 and (nl < 0 or sync < nl):
                # ASCII satirda 0xA5 olmaz: oncesi yarim satirdir, ikili cerceve
                # buradan baslar (icindeki 0x0A satir sonu sanilmasin).
                out.append(("junk", bytes(self.buf[:sync])))
                del self.buf[:sync]
                continue
            if nl < 0:
                if len(self.buf) < LINE_SIZE:
                    break
                out.append(("junk", bytes(self.buf[:1])))  # 64 bayt icinde LF yok: kayik
                del self.buf[:1]
                continue
            chunk = bytes(self.buf[:nl + 1])
            del self.buf[:nl + 1]
            if len(chunk) == LINE_SIZE:
                out.append(("line", chunk[:-1].decode("ascii", "replace").rstrip(" ")))
            else:
                out.append(("junk", chunk))
        return out


class Board:
    def __init__(self, port: str) -> None:
        try:
            import serial  # pyserial
        except ImportError:
            sys.exit("pyserial yok: python -m pip install pyserial")
        try:
            self.ser = serial.Serial(port, BAUD, timeout=0.05)
        except serial.SerialException as e:
            sys.exit(f"{port} acilamadi: {e}\n(Kayit masasi ya da baska bir terminal portu tutuyor olabilir.)")
        self.port = port
        self.reader = FrameReader()
        self.t0 = time.monotonic()
        self.raw: list[str] = []

    def close(self) -> None:
        self.ser.close()

    def _log(self, direction: str, text: str) -> None:
        self.raw.append(f"{time.monotonic() - self.t0:10.3f} {direction} {text}")

    def send(self, typ: int, arg: int) -> None:
        frame = bytes([0xAA, 0x55, typ, arg, (0xAA + 0x55 + typ + arg) & 0xFF])
        self.ser.write(frame)
        self.ser.flush()
        self._log(">", frame.hex(" ").upper())

    def poll(self) -> list[str]:
        data = self.ser.read(self.ser.in_waiting or 1)
        lines = []
        for kind, item in self.reader.feed(data):
            if kind == "line":
                self._log("<", item)
                lines.append(item)
            else:
                self._log("<", f"{kind.upper()} {item.hex(' ').upper()}")
        return lines

    def collect_until(self, done, timeout: float) -> tuple[list[str], str | None]:
        """done(satir) dogru olana kadar gelen satirlari toplar."""
        seen: list[str] = []
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            for line in self.poll():
                seen.append(line)
                if done(line):
                    return seen, line
        return seen, None


# --------------------------------------------------------------------------
# SystemView kaydi (sysview_record.py, ayri surec)
# --------------------------------------------------------------------------

class Recorder:
    def __init__(self, name: str, seconds: float) -> None:
        self.path = TRACE_DIR / f"{name}.SVDat"
        cmd = [sys.executable, "-u", str(SYSVIEW_RECORD), name, "--sure", str(seconds),
               "--cikti", str(TRACE_DIR)]
        self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                     encoding="utf-8", errors="replace")
        self.lines: list[str] = []
        self.ready = threading.Event()
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self) -> None:
        for line in self.proc.stdout:
            line = line.rstrip()
            if line and not line.lstrip().startswith("kalan"):
                self.lines.append(line)
            if "Kayit basladi" in line:
                self.ready.set()

    def wait_ready(self, timeout: float = 40.0) -> bool:
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if self.ready.wait(0.2):
                return True
            if self.proc.poll() is not None:
                return False
        return False

    def finish(self, timeout: float) -> dict:
        try:
            rc = self.proc.wait(timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            rc = None
        self.reader.join(2.0)
        text = "\n".join(self.lines)
        m = re.search(r"Dusen olay \(OVERFLOW\): (\d+)", text)
        return {
            "dosya": str(self.path.relative_to(ROOT)) if self.path.exists() and rc == 0 else None,
            "cikis_kodu": rc,
            "dusen_olay": int(m.group(1)) if m else None,
            "cikti": self.lines,
        }

    def kill(self) -> None:
        if self.proc.poll() is None:
            self.proc.kill()


# --------------------------------------------------------------------------
# Satir cozumu
# --------------------------------------------------------------------------

@dataclass
class QRun:
    run: int
    variant: str
    load: str
    capacity: int
    wait_ticks: int
    service_ms: int
    gap_ms: int
    trace: bool


def parse_qrun(line: str) -> QRun:
    f = line.split(",")
    if len(f) != 9 or f[0] != "QRUN":
        raise RunError(f"QRUN satiri cozulemedi: {line!r}")
    return QRun(int(f[1]), f[2], f[3], int(f[4]), int(f[5]), int(f[6]), int(f[7]), f[8] == "T")


def ack_trace(line: str) -> bool | None:
    """ACK,<seq>,S<n>,<surum>: surumun sonundaki T izleme acik demek."""
    f = line.split(",")
    return ("T" in f[3][1:]) if len(f) == 4 and f[3] else None


def opt_int(s: str) -> int | None:
    return int(s) if s != "" else None


def parse_dump(lines: list[str], run: int, load: str) -> tuple[list[dict], dict[str, int], int | None]:
    rows, counters, end = [], {}, None
    for line in lines:
        f = line.split(",")
        if len(f) < 2 or f[1] != str(run):
            continue
        if f[0] == "QMSG" and len(f) == 9:
            seq = int(f[2])
            rows.append({
                "seq": seq,
                "phase": "art_arda" if load == "yogun" and seq in BURST else "normal",
                "send_result": "accepted" if f[3] == "ok" else "failed",
                "t_gen_us": int(f[4]),
                "send_call_us": int(f[5]),
                "generation_to_receive_us": opt_int(f[6]),
                "service_us": opt_int(f[7]),
                "queue_depth_after_send": opt_int(f[8]),
            })
        elif f[0] == "QCNT" and len(f) == 4:
            counters[f[2]] = int(f[3])
        elif f[0] == "QEND" and len(f) == 3:
            end = int(f[2])
    return rows, counters, end


# --------------------------------------------------------------------------
# Dogrulama ve ozet
# --------------------------------------------------------------------------

def late_seqs(rows: list[dict]) -> list[int]:
    """Servisi deadline'dan gec baslayan (g2r > DEADLINE_US) kabul edilmis mesajlar."""
    return [r["seq"] for r in rows if r["send_result"] == "accepted"
            and r["generation_to_receive_us"] is not None and r["generation_to_receive_us"] > DEADLINE_US]


def seq_ranges(seqs: list[int]) -> str:
    """[12, 13, 14, 20] -> '12-14, 20'"""
    out: list[str] = []
    for s in sorted(seqs):
        if out and int(out[-1].split("-")[-1]) == s - 1:
            out[-1] = f"{out[-1].split('-')[0]}-{s}"
        else:
            out.append(str(s))
    return ", ".join(out)


def checks_for(rows: list[dict], c: dict[str, int], end: int | None, load: str,
               variant: str = "") -> list[dict]:
    acc = [r for r in rows if r["send_result"] == "accepted"]
    fail = [r for r in rows if r["send_result"] == "failed"]
    seqs = sorted(r["seq"] for r in rows)

    def chk(name: str, ok: bool, detail: str = "") -> dict:
        return {"kontrol": name, "tamam": bool(ok), "ayrinti": detail}

    out = [
        chk("44 QMSG, seq 1..44", seqs == list(range(1, ATTEMPTS + 1)), f"{len(rows)} satir"),
        chk("QEND = QMSG sayisi", end == len(rows), f"QEND {end}"),
        chk("attempted = 44", c.get("attempted") == ATTEMPTS, f"{c.get('attempted')}"),
        chk("attempted = accepted + failed",
            c.get("attempted") == c.get("accepted", -1) + c.get("failed", -1),
            f"{c.get('attempted')} = {c.get('accepted')} + {c.get('failed')}"),
        chk("accepted = consumed (kuyruk bosaldi)", c.get("accepted") == c.get("consumed"),
            f"{c.get('accepted')} / {c.get('consumed')}"),
        chk("QMSG ok/fail = sayaclar", len(acc) == c.get("accepted") and len(fail) == c.get("failed"),
            f"ok {len(acc)}, fail {len(fail)}"),
        chk("kabul edilenin gecikmesi var", all(r["generation_to_receive_us"] is not None for r in acc)),
        chk("basarisizin gecikmesi bos (0 degil)",
            all(r["generation_to_receive_us"] is None and r["service_us"] is None for r in fail)),
        chk("sira hatasi yok", c.get("order_error") == 0, f"{c.get('order_error')}"),
        chk("tuketici kosu ortasinda zaman asimina dusmedi", c.get("rx_idle_timeout") == 0,
            f"{c.get('rx_idle_timeout')}"),
    ]
    if load == "normal":
        out.append(chk("normal yukte kayip yok", not fail, f"{len(fail)} kayip"))
    if variant in ARDA_VARIANTS:  # Arda/Arda2: tasarim varsayimlari (arda.h)
        lost_svc = sum(1 for r in acc if r["generation_to_receive_us"] is None)  # kabul edildi, servis edilmedi
        out += [
            chk("Arda: hic kayip yok (gonderimde ve serviste)", not fail and lost_svc == 0,
                f"gonderimde {len(fail)}, serviste {lost_svc}"),
            chk("Arda: port dolmadi, tutamac hatasi yok",
                c.get("pool_empty") == 0 and c.get("pool_alloc_error") == 0 and c.get("pool_free_error") == 0,
                f"dolu {c.get('pool_empty')}, hata {c.get('pool_alloc_error')}/{c.get('pool_free_error')}"),
            chk("Arda: her tutamac geri dondu", c.get("pool_leak") == 0, f"{c.get('pool_leak')}"),
            chk("Arda: doku istegi beklemeden kondu", c.get("dump_retry") == 0, f"{c.get('dump_retry')}"),
            chk("Arda: cerceve kacirilmadi", c.get("frame_miss") == 0, f"{c.get('frame_miss')}"),
        ]
        starts = [r["t_gen_us"] + r["generation_to_receive_us"] for r in sorted(acc, key=lambda r: r["seq"])
                  if r["generation_to_receive_us"] is not None]
        inv = sum(1 for a, b in zip(starts, starts[1:]) if b < a)
        out.append(chk("Arda: servis baslari sira numarasini izledi (KAN-03)", inv == 0, f"{inv} ters"))
        late = late_seqs(rows)
        out.append(chk(f"Arda: deadline ({DEADLINE_MS} ms) kacmadi (DL-04)", not late,
                       f"{len(late)} mesaj" + (f": {seq_ranges(late)}" if late else "")))
        if "deadline_miss" in c:  # port katmanli firmware (kayit 10, 11)
            out += [
                chk("Arda: deadline sayaci kayitlarla tutarli", c["deadline_miss"] == len(late),
                    f"sayac {c['deadline_miss']}, kayit {len(late)}"),
                chk("Arda: durum portu bayat okunmadi", c.get("status_stale") == 0, f"{c.get('status_stale')}"),
                chk("Arda: durum portu her girisimden once okundu", c.get("status_reads") == ATTEMPTS,
                    f"{c.get('status_reads')}"),
                # Adim 4 firmware'i: kartin kurye sayisi kayda uymali (eski firmware'de sayac yok).
                chk(f"Arda: {ARDA_COURIERS} kurye (hat), art arda bolumde hepsi yolda",
                    c.get("couriers") == ARDA_COURIERS and c.get("courier_busy_max") == ARDA_COURIERS,
                    f"kurye {c.get('couriers')}, ayni anda en cok {c.get('courier_busy_max')}"),
                # Adim 5 firmware'i: toplu servis siniri kayda uymali.
                chk(f"Arda: seferde en cok {ARDA_BATCH} mesaj, art arda bolumde dolu",
                    c.get("batch") == ARDA_BATCH and c.get("batch_max") == ARDA_BATCH,
                    f"sinir {c.get('batch')}, olculen {c.get('batch_max')}"),
                # Adim 6 firmware'i: cerceve tick kenarindan ARDA_FRAME_PHASE_US sonra baslamali.
                chk(f"Arda: cerceve fazi {ARDA_FRAME_PHASE_US} us (tick kenarindan), sapma <= {FRAME_PHASE_TOL_US} us",
                    c.get("frame_phase_us") == ARDA_FRAME_PHASE_US
                    and c.get("frame_phase_min_us", -1) >= ARDA_FRAME_PHASE_US
                    and c.get("frame_phase_max_us", 1 << 31) <= ARDA_FRAME_PHASE_US + FRAME_PHASE_TOL_US,
                    f"ayar {c.get('frame_phase_us')}, olculen {c.get('frame_phase_min_us')}..{c.get('frame_phase_max_us')} us"),
            ]
    return out


def mean(xs: list[float]) -> float | None:
    return sum(xs) / len(xs) if xs else None


def p95(xs: list[float]) -> float | None:
    """En yakin sira yontemi (nearest-rank): gozlenen bir degerdir."""
    if not xs:
        return None
    s = sorted(xs)
    return s[max(0, math.ceil(0.95 * len(s)) - 1)]


def summarize(rows: list[dict], c: dict[str, int]) -> dict:
    acc = [r for r in rows if r["send_result"] == "accepted"]
    fail = [r for r in rows if r["send_result"] == "failed"]
    g2r = [r["generation_to_receive_us"] for r in acc if r["generation_to_receive_us"] is not None]
    svc = [r["service_us"] for r in acc if r["service_us"] is not None]
    burst = [r for r in rows if r["phase"] == "art_arda"]
    return {
        "attempted": c.get("attempted"), "accepted": c.get("accepted"), "failed": c.get("failed"),
        "consumed": c.get("consumed"), "queue_max": c.get("queue_max"),
        "art_arda_accepted": sum(r["send_result"] == "accepted" for r in burst),
        "art_arda_failed": sum(r["send_result"] == "failed" for r in burst),
        "send_ok_mean_us": mean([r["send_call_us"] for r in acc]),
        "send_ok_max_us": max((r["send_call_us"] for r in acc), default=None),
        "send_fail_mean_us": mean([r["send_call_us"] for r in fail]),
        "send_fail_max_us": max((r["send_call_us"] for r in fail), default=None),
        "producer_send_total_ms": sum(r["send_call_us"] for r in rows) / 1000.0,
        "g2r_mean_us": mean(g2r), "g2r_p95_us": p95(g2r), "g2r_max_us": max(g2r, default=None),
        "deadline_miss": len(late_seqs(rows)),
        "service_mean_us": mean(svc),
        "run_ms": c["run_us"] / 1000.0 if "run_us" in c else None,
        "stack_hwm_producer": c.get("stack_hwm_producer"),
        "stack_hwm_consumer": c.get("stack_hwm_consumer"),
    }


def fmt_ms(us: float | None) -> str:
    return "-" if us is None else f"{us / 1000:.2f} ms"


def git_state() -> dict:
    def git(*a: str) -> str | None:
        try:
            r = subprocess.run(["git", "-C", str(ROOT), *a], capture_output=True, text=True, timeout=10)
            return r.stdout.strip() if r.returncode == 0 else None
        except (OSError, subprocess.TimeoutExpired):
            return None
    status = git("status", "--porcelain")
    return {"commit": git("rev-parse", "--short", "HEAD"), "degisiklik_var": bool(status) if status is not None else None}


def file_sha256(p: Path) -> str | None:
    import hashlib
    return hashlib.sha256(p.read_bytes()).hexdigest()[:16] if p.exists() else None


SUMMARY_FIELDS = [
    "kayit", "tekrar", "variant", "load", "trace", "capacity", "send_wait_ticks", "attempted",
    "accepted", "failed", "consumed", "queue_max", "art_arda_accepted", "art_arda_failed",
    "send_ok_mean_us", "send_ok_max_us", "send_fail_mean_us", "send_fail_max_us",
    "producer_send_total_ms", "g2r_mean_us", "g2r_p95_us", "g2r_max_us", "deadline_miss", "service_mean_us",
    "run_ms", "kontroller_tamam", "baslangic", "dosya",
]


def write_summary() -> int:
    rows = []
    for p in sorted(MEAS_DIR.glob("kayit*_meta.json")):
        m = json.loads(p.read_text(encoding="utf-8"))
        k = m["kosu"]
        row = {"kayit": k["kayit"], "tekrar": k["tekrar"], "variant": k["variant"], "load": k["load"],
               "trace": "acik" if k["trace"] else "kapali", "capacity": k["capacity"],
               "send_wait_ticks": k["send_wait_ticks"],
               "kontroller_tamam": all(x["tamam"] for x in m["kontroller"]),
               "baslangic": k["baslangic"], "dosya": p.name.replace("_meta.json", "")}
        for key, v in m["ozet"].items():
            if key in SUMMARY_FIELDS:
                row[key] = round(v, 1) if isinstance(v, float) else v
        rows.append(row)
    rows.sort(key=lambda r: (r["kayit"], r["tekrar"]))
    MEAS_DIR.mkdir(parents=True, exist_ok=True)
    with SUMMARY_CSV.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=SUMMARY_FIELDS)
        w.writeheader()
        w.writerows(rows)
    return len(rows)


# --------------------------------------------------------------------------
# Kosu
# --------------------------------------------------------------------------

def archive(paths: list[Path]) -> None:
    """Ayni adli eski dosyalari silmeden eski/<zaman>/ altina tasir."""
    existing = [p for p in paths if p.exists()]
    if not existing:
        return
    stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    for p in existing:
        dest = p.parent / "eski" / stamp
        dest.mkdir(parents=True, exist_ok=True)
        p.rename(dest / p.name)


def run_one(board: Board, kayit: str, tekrar: int, n_tekrar: int, systemview: bool) -> bool:
    variant, load, trace_expected = RECORDS[kayit]
    base = f"kayit{kayit}_{vname(variant)}_{load}_iz_{'acik' if trace_expected else 'kapali'}"
    trace_name = f"kayit{kayit}_{vname(variant)}_{load}"
    if tekrar > 1:  # ilk kosu eksiz: web arayuzuyle (interface/) ayni adlandirma
        base += f"_t{tekrar}"
        trace_name += f"_t{tekrar}"
    print(f"\nKayit {kayit}: {vname(variant)}/{load}, iz {'acik' if trace_expected else 'kapali'}"
          + (f", tekrar {tekrar}/{n_tekrar}" if n_tekrar > 1 else ""))
    board.raw.clear()

    # 1) Telemetri sussun (S0) ve kartin izleme durumu ACK'ten okunsun.
    ack = None
    for _ in range(2):
        board.send(CMD_SCENARIO, 0)
        _, ack = board.collect_until(lambda l: l.startswith("ACK,"), ACK_TIMEOUT_S)
        if ack:
            break
    if not ack:
        raise RunError("Kart yanit vermiyor (ACK yok): port, baud, kablo ve kartin calistigini kontrol edin.")
    board.collect_until(lambda l: False, 0.3)  # kuyrukta kalan satirlar
    trace_on = ack_trace(ack)
    if trace_on is not None and trace_on != trace_expected:
        raise RunError(
            f"Kartin izleme durumu ({'acik' if trace_on else 'kapali'}) kayit {kayit} ile uyusmuyor. "
            f"Firmware'i USE_SYSVIEW {1 if trace_expected else 0} ile derleyip yukleyin "
            "(firmware/Core/Inc/FreeRTOSConfig.h).")

    # 2) SystemView kaydi kosudan once baslamali.
    rec = None
    if systemview and trace_expected:
        archive([TRACE_DIR / f"{trace_name}.SVDat"])
        rec = Recorder(trace_name, SYSVIEW_SECONDS)
        if not rec.wait_ready():
            rec.kill()
            raise RunError("SystemView kaydi baslamadi:\n  " + "\n  ".join(rec.lines[-8:]))

    try:
        # 3) Kosu
        arg = VARIANTS.index(variant) | (QRUN_LOAD_BURST if load == "yogun" else 0)
        started = dt.datetime.now().astimezone().isoformat(timespec="seconds")
        board.send(CMD_QRUN, arg)
        _, first = board.collect_until(lambda l: l.startswith(("QRUN,", "QBUSY,")), QRUN_TIMEOUT_S)
        if first and first.startswith("QBUSY,"):
            print("  Kart onceki kosuyu bitiriyor, bekleniyor...")
            board.collect_until(lambda l: l.startswith("QEND,"), RUN_TIMEOUT_S)
            board.send(CMD_QRUN, arg)
            _, first = board.collect_until(lambda l: l.startswith(("QRUN,", "QBUSY,")), QRUN_TIMEOUT_S)
        if not first or not first.startswith("QRUN,"):
            raise RunError(f"Kosu baslamadi (QRUN yok, gelen: {first!r}). Firmware Hafta 3 surumu mu?")
        q = parse_qrun(first)
        print(f"  Kart kosusu {q.run}: kapasite {q.capacity}, bekleme {q.wait_ticks} tick, "
              f"servis {q.service_ms} ms, aralik {q.gap_ms} ms, iz {'acik' if q.trace else 'kapali'}")
        lines, end_line = board.collect_until(lambda l: l.startswith(f"QEND,{q.run},"), RUN_TIMEOUT_S)
        if end_line is None:
            raise RunError(f"Kosu {RUN_TIMEOUT_S:.0f} s icinde bitmedi (QEND yok).")
    finally:
        sv = rec.finish(SYSVIEW_SECONDS + 30) if rec else None

    if (q.variant, q.load) != (variant, load) or q.trace != trace_expected:
        raise RunError(f"Kart farkli bir kosu bildirdi: {first}")

    rows, counters, end = parse_dump(lines, q.run, load)
    checks = checks_for(rows, counters, end, load, variant)
    ozet = summarize(rows, counters)
    if sv is not None and sv["dusen_olay"]:
        checks.append({"kontrol": "SystemView olay dusurmedi", "tamam": False,
                       "ayrinti": f"{sv['dusen_olay']} olay dustu"})

    # 4) Dosyalar
    MEAS_DIR.mkdir(parents=True, exist_ok=True)
    paths = {k: MEAS_DIR / f"{base}{suf}" for k, suf in
             [("csv", ".csv"), ("cnt", "_counters.csv"), ("raw", "_raw.txt"), ("meta", "_meta.json")]}
    archive(list(paths.values()))

    fields = ["kayit", "tekrar", "board_run", "variant", "load", "trace", "seq", "phase", "send_result",
              "t_gen_us", "send_call_us", "generation_to_receive_us", "service_us", "queue_depth_after_send"]
    with paths["csv"].open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for r in sorted(rows, key=lambda r: r["seq"]):
            w.writerow({"kayit": kayit, "tekrar": tekrar, "board_run": q.run, "variant": variant, "load": load,
                        "trace": "acik" if q.trace else "kapali", **r})
    with paths["cnt"].open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["name", "value"])
        w.writerows(counters.items())
    paths["raw"].write_text("\n".join(board.raw) + "\n", encoding="utf-8")
    meta = {
        "kosu": {"kayit": kayit, "tekrar": tekrar, "variant": variant, "load": load, "trace": q.trace,
                 "board_run": q.run, "capacity": q.capacity, "send_wait_ticks": q.wait_ticks,
                 "service_ms": q.service_ms, "gap_ms": q.gap_ms, "attempts": ATTEMPTS,
                 "art_arda": f"{BURST.start}..{BURST.stop - 1}" if load == "yogun" else None,
                 "baslangic": started},
        "seri": {"port": board.port, "baud": BAUD, "qrun": first, "ack": ack},
        "yazilim": {**git_state(), "yerel_derleme_sha256_16": file_sha256(FIRMWARE_OUT),
                    "not": "yerel_derleme: bu bilgisayardaki Debug .out; karta yuklu olanla ayni oldugu ayrica dogrulanmali"},
        "systemview": sv,
        "kontroller": checks,
        "ozet": ozet,
    }
    paths["meta"].write_text(json.dumps(meta, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    # 5) Ekran ozeti
    print(f"  kabul {ozet['accepted']}/{ozet['attempted']}, kayip {ozet['failed']}, "
          f"tuketilen {ozet['consumed']}, kuyruk en fazla {ozet['queue_max']}"
          + (f", art arda bolum: {ozet['art_arda_accepted']} kabul / {ozet['art_arda_failed']} kayip"
             if load == "yogun" else ""))
    print(f"  send_call: kabul ort {fmt_ms(ozet['send_ok_mean_us'])} / en fazla {fmt_ms(ozet['send_ok_max_us'])}"
          f"; basarisiz ort {fmt_ms(ozet['send_fail_mean_us'])}")
    print(f"  uretim->alma: ort {fmt_ms(ozet['g2r_mean_us'])}, p95 {fmt_ms(ozet['g2r_p95_us'])}, "
          f"en fazla {fmt_ms(ozet['g2r_max_us'])}; kosu {fmt_ms(counters.get('run_us'))}")
    if variant in ARDA_VARIANTS:
        late = late_seqs(rows)
        print(f"  deadline ({DEADLINE_MS} ms): {len(late)} mesaj kacti" + (f" ({seq_ranges(late)})" if late else ""))
        if "status_reads" in counters:
            print(f"  durum portu: yazma {counters.get('status_writes')}, okuma {counters.get('status_reads')}, "
                  f"atlanan {counters.get('status_missed')}, bayat {counters.get('status_stale')}, "
                  f"yas en cok {counters.get('status_age_max_us')} us, "
                  f"Producer'in gordugu birikme en cok {counters.get('status_backlog_max')}")
        if "couriers" in counters:
            print(f"  kurye (hat): {counters.get('couriers')}, ayni anda yolda en cok {counters.get('courier_busy_max')}")
        if "batch" in counters:
            print(f"  toplu servis: seferde en cok {counters.get('batch')}, olculen {counters.get('batch_max')}")
        if "frame_phase_us" in counters:
            print(f"  cerceve fazi: ayar {counters.get('frame_phase_us')} us, olculen "
                  f"{counters.get('frame_phase_min_us')}..{counters.get('frame_phase_max_us')} us")
    if sv is not None:
        print(f"  SystemView: {sv['dosya'] or 'KAYDEDILEMEDI'}"
              + (f", dusen olay {sv['dusen_olay']}" if sv["dusen_olay"] is not None else ""))
    bad = [x for x in checks if not x["tamam"]]
    if bad:
        for x in bad:
            print(f"  KONTROL BASARISIZ: {x['kontrol']} ({x['ayrinti']})")
    else:
        print(f"  Kontroller tamam ({len(checks)}).")
    print(f"  Dosyalar: measurements/{base}{{.csv,_counters.csv,_raw.txt,_meta.json}}")
    return not bad


def parse_records(spec: str) -> list[str]:
    out: list[str] = []
    for part in spec.split(","):
        part = part.strip()
        if "-" in part:
            a, b = (int(x) for x in part.split("-", 1))
            out += [f"{i:02d}" for i in range(a, b + 1)]
        elif part:
            out.append(f"{int(part):02d}")
    bad = [k for k in out if k not in RECORDS]
    if bad:
        sys.exit(f"Gecersiz kayit: {', '.join(bad)} (01..07)")
    return out


def list_ports() -> None:
    try:
        from serial.tools import list_ports as lp
    except ImportError:
        sys.exit("pyserial yok: python -m pip install pyserial")
    for p in lp.comports():
        print(f"  {p.device:8} {p.description}")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="seri port, orn. COM5")
    ap.add_argument("--kayit", default="01-06", help="kayit numaralari, orn. 01-06, 07, 04,05 (varsayilan 01-06)")
    ap.add_argument("--tekrar", type=int, default=1, help="her kaydin tekrar sayisi (varsayilan 1)")
    ap.add_argument("--systemview", action="store_true", help="iz acik kosularda J-Link ile .SVDat kaydi al")
    ap.add_argument("--ozet", action="store_true", help="yalnizca measurements/queue_summary.csv'yi yeniden yaz")
    ap.add_argument("--portlar", action="store_true", help="seri portlari listele")
    a = ap.parse_args()

    if a.portlar:
        list_ports()
        return
    if a.ozet:
        print(f"{write_summary()} kosu -> {SUMMARY_CSV.relative_to(ROOT)}")
        return
    if not a.port:
        ap.error("--port gerekli (portlari gormek icin --portlar)")
    if a.tekrar < 1:
        ap.error("--tekrar en az 1")

    records = parse_records(a.kayit)
    board = Board(a.port)
    results: list[tuple[str, int, str]] = []
    try:
        for kayit in records:
            for t in range(1, a.tekrar + 1):
                try:
                    ok = run_one(board, kayit, t, a.tekrar, a.systemview)
                    results.append((kayit, t, "tamam" if ok else "KONTROL BASARISIZ"))
                except RunError as e:
                    print(f"  HATA: {e}")
                    results.append((kayit, t, "HATA"))
    except KeyboardInterrupt:
        print("\nDurduruldu.")
    finally:
        board.close()
        n = write_summary()

    print(f"\nOzet tablosu: {SUMMARY_CSV.relative_to(ROOT)} ({n} kosu)")
    for kayit, t, status in results:
        print(f"  kayit {kayit}" + (f" t{t}" if a.tekrar > 1 else "") + f": {status}")
    sys.exit(0 if results and all(s == "tamam" for _, _, s in results) else 1)


if __name__ == "__main__":
    main()

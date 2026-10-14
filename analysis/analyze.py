#!/usr/bin/env python3
"""
analyze.py - Hafta 3 queue deneyi analizi (odev-03)

Girdi: measurements/kayitNN_<surum>_<yuk>_iz_<acik|kapali>[_tN]_meta.json ve
ayni adli .csv (queue_experiment.py ya da interface/ kayit masasi ciktilari).

Cikti:
  measurements/queue_summary.csv            kosu basina ozet (queue_experiment.py --ozet ile ayni)
  analysis/karsilastirma.md                 kosu tablosu, tek etkenli karsilastirmalar
                                            (A->B kapasite, A->C bekleme, iz acik->kapali)
                                            ve model kontrolu; rapora kopyalanabilir
  analysis/plots/zaman_cizelgesi_yogun.png  yogun kosular: her girisim bir satir
  analysis/plots/kabul_kayip.png            kosu basina kabul / kayip
  analysis/plots/doluluk_gecikme.png        gonderimden sonraki doluluk -> uretim->alma,
                                            model cizgileriyle

Model (S = olculen ortalama servis suresi, d = basarili gonderimden hemen
sonra kuyruktaki mesaj sayisi):
  tuketici bosken kuyruga giren mesaj         uretim->alma ~ (d - 1) * S
  tuketici bir mesaji servis ederken giren    uretim->alma ~ d * S
A/B'de art arda bolum bos bir tuketiciye gelir (birinci durum). C'de bloklanan
gonderim, tuketici bir mesaji servise aldigi anda gecer (ikinci durum).

Ayni kaydin tekrarlari (_t2, _t3) karsilastirmalarda ortalanir; zaman
cizelgesinde ilk tekrar cizilir.

Kullanim (depo kokunden ya da analysis/ icinden):
    python analysis/analyze.py
    python analysis/analyze.py --measurements measurements --plots analysis/plots

Bagimliliklar: pandas, matplotlib (pip install pandas matplotlib)
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
from dataclasses import dataclass
from pathlib import Path

import matplotlib

matplotlib.use("Agg")  # pencere acmadan dosyaya ciz
import matplotlib.pyplot as plt
import pandas as pd

import queue_experiment as qe  # ayni ozet tablosu (queue_summary.csv)

HERE = Path(__file__).resolve().parent
LOAD_TEXT = {"normal": "normal", "yogun": "yoğun"}
BURST_FIRST, BURST_LAST = 11, 34
COLORS = {"block": "#8f7fe0", "wait": "#e0a030", "svc": "#4c9aff", "fail": "#e05050",
          "acc_normal": "#4c9aff", "acc_burst": "#2fbf9a"}

# Tek etkenli karsilastirmalar (odev-03): (baslik, once, sonra)
PAIRS = [
    ("A → B, kapasite 4 → 16 (normal yük)", "01", "02"),
    ("A → B, kapasite 4 → 16 (yoğun yük)", "04", "05"),
    ("A → C, bekleme 0 → 20 ms (normal yük)", "01", "03"),
    ("A → C, bekleme 0 → 20 ms (yoğun yük)", "04", "06"),
    ("İz açık → kapalı (A, yoğun yük)", "04", "07"),
]


@dataclass
class Run:
    base: str
    meta: dict
    df: pd.DataFrame

    @property
    def kosu(self) -> dict:
        return self.meta["kosu"]

    @property
    def ozet(self) -> dict:
        return self.meta["ozet"]

    @property
    def label(self) -> str:
        k = self.kosu
        t = f" t{k['tekrar']}" if k["tekrar"] > 1 else ""
        return f"{k['kayit']}{t} · {qe.vname(k['variant'])}/{LOAD_TEXT[k['load']]}"

    @property
    def checks_ok(self) -> bool:
        return all(c["tamam"] for c in self.meta["kontroller"])


def load_runs(folder: Path) -> list[Run]:
    runs = []
    for meta_path in sorted(folder.glob("kayit*_meta.json")):
        base = meta_path.name[: -len("_meta.json")]
        csv_path = folder / f"{base}.csv"
        if not csv_path.exists():
            print(f"uyari: {csv_path.name} yok, {base} atlandi")
            continue
        meta = json.loads(meta_path.read_text(encoding="utf-8"))
        df = pd.read_csv(csv_path, dtype={"kayit": str})
        for c in ["generation_to_receive_us", "service_us", "queue_depth_after_send"]:
            df[c] = pd.to_numeric(df[c], errors="coerce")  # bos -> NaN (basarisiz gonderim)
        runs.append(Run(base, meta, df.sort_values("seq").reset_index(drop=True)))
    return sorted(runs, key=lambda r: (r.kosu["kayit"], r.kosu["tekrar"]))


# --------------------------------------------------------------------------
# Bicimleme (rapor Turkce: ondalik virgul)
# --------------------------------------------------------------------------

def num(v: float | None, digits: int = 2) -> str:
    if v is None or (isinstance(v, float) and pd.isna(v)):
        return "—"
    text = f"{v:,.{digits}f}".replace(",", "X").replace(".", ",").replace("X", ".")
    return text.replace("-", "−")


def dur(us: float | None) -> str:
    """1 ms altinda us, ustunde ms (A/B'de send_call birkac us, C'de ~20 ms)."""
    if us is None or (isinstance(us, float) and pd.isna(us)):
        return "—"
    return f"{num(us, 1 if us < 10 else 0)} µs" if us < 1000 else f"{num(us / 1000)} ms"


def md_table(header: list[str], rows: list[list[str]]) -> str:
    out = ["| " + " | ".join(header) + " |", "|" + "|".join("---" for _ in header) + "|"]
    out += ["| " + " | ".join(r) + " |" for r in rows]
    return "\n".join(out)


# --------------------------------------------------------------------------
# Karsilastirma
# --------------------------------------------------------------------------

METRICS = [  # (anahtar, etiket, bicim)
    ("accepted", "kabul", lambda v: num(v, 1 if v % 1 else 0)),
    ("failed", "kayıp", lambda v: num(v, 1 if v % 1 else 0)),
    ("art_arda_failed", "art arda bölümde kayıp", lambda v: num(v, 1 if v % 1 else 0)),
    ("queue_max", "kuyruk tepe doluluğu", lambda v: num(v, 1 if v % 1 else 0)),
    ("g2r_mean_us", "üretim → alma, ort.", dur),
    ("g2r_p95_us", "üretim → alma, p95", dur),
    ("g2r_max_us", "üretim → alma, maks", dur),
    ("send_ok_mean_us", "send_call, kabul ort.", dur),
    ("send_fail_mean_us", "send_call, başarısız ort.", dur),
    ("producer_send_total_ms", "üretici gönderimde toplam", lambda v: dur(v * 1000)),
    ("run_ms", "koşu süresi", lambda v: dur(v * 1000)),
]


def per_record(runs: list[Run]) -> dict[str, dict]:
    """Kayit numarasi -> tekrarlarin ortalamasi (n ile)."""
    out: dict[str, dict] = {}
    for kayit in sorted({r.kosu["kayit"] for r in runs}):
        group = [r for r in runs if r.kosu["kayit"] == kayit]
        agg = {"n": len(group), "run": group[0]}
        for key, *_ in METRICS:
            vals = [r.ozet.get(key) for r in group if r.ozet.get(key) is not None]
            agg[key] = sum(vals) / len(vals) if vals else None
        out[kayit] = agg
    return out


def pair_section(title: str, a: str, b: str, rec: dict[str, dict]) -> str:
    if a not in rec or b not in rec:
        missing = ", ".join(k for k in (a, b) if k not in rec)
        return f"### {title}: {a} → {b}\n\nKayıt {missing} henüz alınmadı.\n"
    ra, rb = rec[a], rec[b]
    rows = []
    for key, label, fmt in METRICS:
        va, vb = ra[key], rb[key]
        if va is None and vb is None:
            continue
        if va is not None and vb is not None:
            diff = vb - va
            sign = "+" if diff > 0 else ("−" if diff < 0 else "±")
            dtext = f"{sign}{fmt(abs(diff))}"
        else:
            dtext = "—"
        rows.append([label, fmt(va) if va is not None else "—", fmt(vb) if vb is not None else "—", dtext])
    n = f" (ortalama: {a} n={ra['n']}, {b} n={rb['n']})" if ra["n"] > 1 or rb["n"] > 1 else ""
    return f"### {title}: {a} → {b}{n}\n\n" + md_table(["ölçü", a, b, "fark"], rows) + "\n"


def model_section(runs: list[Run]) -> str:
    rows = []
    for r in runs:
        if r.kosu["load"] != "yogun":
            continue
        acc = r.df[r.df["send_result"] == "accepted"].dropna(subset=["generation_to_receive_us"])
        s = r.ozet["service_mean_us"]
        cap = r.kosu["capacity"]
        if r.kosu["send_wait_ticks"] == 0:
            model = (cap - 1) * s
            rule = f"(K − 1)·S = {cap - 1} × {dur(s)}"
        else:
            model = cap * s
            rule = f"K·S = {cap} × {dur(s)}"
        rows.append([r.label, rule, dur(model), dur(acc["generation_to_receive_us"].max()),
                     num(100 * (acc["generation_to_receive_us"].max() - model) / model, 1) + " %"])
        fails = r.df[r.df["send_result"] == "failed"]
        if r.kosu["send_wait_ticks"] > 0 and not fails.empty:
            wait_us = r.kosu["send_wait_ticks"] * 1000  # 1 kHz tick
            rows.append([r.label, f"başarısız send_call ≈ W = {r.kosu['send_wait_ticks']} tick",
                         dur(wait_us), dur(fails["send_call_us"].mean()),
                         num(100 * (fails["send_call_us"].mean() - wait_us) / wait_us, 1) + " %"])
    if not rows:
        return "Yoğun yük koşusu yok.\n"
    return md_table(["koşu", "model", "model değeri", "ölçülen (maks / ort.)", "sapma"], rows) + "\n"


def write_report(runs: list[Run], path: Path) -> str:
    rec = per_record(runs)
    run_rows = []
    for r in runs:
        o, k = r.ozet, r.kosu
        burst = f"{o['art_arda_accepted']} / {o['art_arda_failed']}" if k["load"] == "yogun" else "—"
        run_rows.append([
            r.label, "açık" if k["trace"] else "kapalı", f"{k['capacity']} / {k['send_wait_ticks']} tick",
            str(o["accepted"]), str(o["failed"]), burst, f"{o['queue_max']}/{k['capacity']}",
            dur(o["send_fail_mean_us"]), dur(o["g2r_mean_us"]), dur(o["g2r_p95_us"]), dur(o["g2r_max_us"]),
            dur(o["producer_send_total_ms"] * 1000), dur(o["run_ms"] * 1000) if o["run_ms"] is not None else "—",
            "tamam" if r.checks_ok else "HATA",
        ])
    text = [
        "# Hafta 3 queue deneyi: karşılaştırma",
        "",
        f"`analysis/analyze.py` ile üretildi ({dt.datetime.now().astimezone().isoformat(timespec='seconds')}). "
        "Kaynak: `measurements/kayit*_meta.json` ve `.csv`. Süreler kartın TIM2 sayacından (1 µs).",
        "",
        "## Koşular",
        "",
        md_table(["koşu", "iz", "kapasite / bekleme", "kabul", "kayıp", "art arda kabul / kayıp", "kuyruk tepe",
                  "başarısız send_call ort.", "üretim→alma ort.", "p95", "maks", "üretici gönderimde",
                  "koşu", "kontroller"], run_rows),
        "",
        "## Tek etkenli karşılaştırmalar",
        "",
        *[pair_section(t, a, b, rec) for t, a, b in PAIRS],
        "## Model kontrolü",
        "",
        "S: koşunun ölçülen ortalama servis süresi, K: kapasite, W: gönderme beklemesi. "
        "A/B'de art arda bölüm boş bir tüketiciye gelir: K. mesaj önündeki K − 1 mesajın servisini bekler. "
        "C'de bloklanan gönderim tüketici bir mesajı servise aldığı anda geçer: önünde K − 1 mesaj ve "
        "serviste bir mesaj vardır.",
        "",
        model_section(runs),
        "Gözlenen maksimum bir worst-case garantisi değildir; bu yük tarifi ve bu kartta ölçülen değerdir.",
        "",
    ]
    out = "\n".join(text)
    path.write_text(out, encoding="utf-8")
    return out


# --------------------------------------------------------------------------
# Grafikler
# --------------------------------------------------------------------------

def plot_timelines(runs: list[Run], out: Path) -> bool:
    chosen = {}
    for r in runs:  # her yogun kayittan ilk tekrar
        if r.kosu["load"] == "yogun" and r.kosu["kayit"] not in chosen:
            chosen[r.kosu["kayit"]] = r
    if not chosen:
        return False
    items = list(chosen.values())
    end_ms = max(
        ((r.df["t_gen_us"] + r.df["generation_to_receive_us"].fillna(0) + r.df["service_us"].fillna(0)).max()
         for r in items)) / 1000
    fig, axes = plt.subplots(len(items), 1, figsize=(11, 3.6 * len(items)), sharex=True, squeeze=False)
    for ax, r in zip(axes[:, 0], items):
        ax.axhspan(BURST_FIRST - 0.5, BURST_LAST + 0.5, color=COLORS["wait"], alpha=0.08, lw=0)
        for _, m in r.df.iterrows():
            t, send = m["t_gen_us"] / 1000, m["send_call_us"] / 1000
            y = m["seq"]
            if m["send_result"] == "failed":
                if send >= 1:
                    ax.broken_barh([(t, send)], (y - 0.35, 0.7), color=COLORS["fail"])
                ax.plot(t + send, y, marker="x", color=COLORS["fail"], ms=4, mew=1.2)
                continue
            g2r, svc = m["generation_to_receive_us"] / 1000, m["service_us"] / 1000
            if send >= 0.5:
                ax.broken_barh([(t, send)], (y - 0.35, 0.7), color=COLORS["block"])
            ax.broken_barh([(t + send, max(g2r - send, 0))], (y - 0.35, 0.7), color=COLORS["wait"])
            ax.broken_barh([(t + g2r, svc)], (y - 0.35, 0.7), color=COLORS["svc"])
        o, k = r.ozet, r.kosu
        ax.set_title(f"Kayıt {r.label} · kapasite {k['capacity']}, bekleme {k['send_wait_ticks']} tick, "
                     f"iz {'açık' if k['trace'] else 'kapalı'} — {o['accepted']} kabul / {o['failed']} kayıp, "
                     f"en uzun üretim→alma {dur(o['g2r_max_us'])}", fontsize=10, loc="left")
        ax.set_ylim(len(r.df) + 0.8, 0.2)
        ax.set_yticks([1, BURST_FIRST, 20, 30, BURST_LAST, 44])
        ax.set_ylabel("seq")
        ax.grid(axis="x", alpha=0.3)
    axes[-1, 0].set_xlabel("üretim anından itibaren zaman (ms)")
    axes[-1, 0].set_xlim(0, end_ms * 1.02)
    handles = [plt.Rectangle((0, 0), 1, 1, color=COLORS[c]) for c in ("block", "wait", "svc", "fail")]
    fig.legend(handles, ["gönderimde bloklu", "kuyrukta bekleme", "servis (20 ms)", "başarısız gönderim (× bitiş)"],
               loc="upper center", ncol=4, fontsize=9, frameon=False)
    fig.tight_layout(rect=(0, 0, 1, 0.97))
    fig.savefig(out / "zaman_cizelgesi_yogun.png", dpi=150)
    plt.close(fig)
    return True


def plot_accept_loss(runs: list[Run], out: Path) -> bool:
    if not runs:
        return False
    labels = [r.label + ("\niz kapalı" if not r.kosu["trace"] else "") for r in runs]
    acc_burst = [r.ozet["art_arda_accepted"] if r.kosu["load"] == "yogun" else 0 for r in runs]
    acc_norm = [r.ozet["accepted"] - b for r, b in zip(runs, acc_burst)]
    failed = [r.ozet["failed"] for r in runs]
    x = range(len(runs))
    fig, ax = plt.subplots(figsize=(max(8, 1.25 * len(runs)), 4.8))
    ax.bar(x, acc_norm, color=COLORS["acc_normal"], label="kabul (normal bölüm)")
    ax.bar(x, acc_burst, bottom=acc_norm, color=COLORS["acc_burst"], label="kabul (art arda bölüm)")
    ax.bar(x, failed, bottom=[a + b for a, b in zip(acc_norm, acc_burst)], color=COLORS["fail"],
           label="başarısız gönderim (kayıp)")
    for i, r in enumerate(runs):
        ax.text(i, 44.6, f"{r.ozet['accepted']}/{r.ozet['failed']}", ha="center", va="bottom", fontsize=9)
    ax.set_xticks(list(x), labels, fontsize=9)
    ax.set_ylim(0, 50)
    ax.set_ylabel("gönderme girişimi (toplam 44)")
    ax.set_title("Koşu başına kabul / kayıp", fontsize=11, loc="left")
    ax.legend(fontsize=8, loc="lower right")
    fig.tight_layout()
    fig.savefig(out / "kabul_kayip.png", dpi=150)
    plt.close(fig)
    return True


def plot_depth_latency(runs: list[Run], out: Path) -> bool:
    pts = []
    for r in runs:
        acc = r.df[r.df["send_result"] == "accepted"].dropna(subset=["generation_to_receive_us"])
        if not acc.empty:
            pts.append((r, acc))
    if not pts:
        return False
    s_ms = sum(r.ozet["service_mean_us"] for r, _ in pts) / len(pts) / 1000
    d_max = int(max(acc["queue_depth_after_send"].max() for _, acc in pts))
    fig, ax = plt.subplots(figsize=(9, 5.2))
    markers = {"A": "o", "B": "s", "C": "^"}
    for r, acc in pts:
        ax.scatter(acc["queue_depth_after_send"], acc["generation_to_receive_us"] / 1000, s=26, alpha=0.75,
                   marker=markers.get(r.kosu["variant"], "o"), label=r.label + ("" if r.kosu["trace"] else ", iz kapalı"))
    d = list(range(1, d_max + 1))
    ax.plot(d, [(v - 1) * s_ms for v in d], color="0.35", lw=1, ls="--",
            label=f"(d − 1)·S: tüketici boşken (S = {num(s_ms)} ms)")
    ax.plot(d, [v * s_ms for v in d], color="0.35", lw=1, ls=":", label="d·S: bir mesaj serviste (C)")
    ax.set_xlabel("d = başarılı gönderimden sonra kuyruktaki mesaj")
    ax.set_ylabel("üretim → alma (ms)")
    ax.set_title("Kuyruk doluluğu → bekleme", fontsize=11, loc="left")
    ax.set_xticks(d)
    ax.grid(alpha=0.3)
    ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(out / "doluluk_gecikme.png", dpi=150)
    plt.close(fig)
    return True


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--measurements", type=Path, default=HERE.parent / "measurements")
    ap.add_argument("--plots", type=Path, default=HERE / "plots")
    ap.add_argument("--rapor", type=Path, default=HERE / "karsilastirma.md")
    args = ap.parse_args()

    runs = load_runs(args.measurements)
    if not runs:
        print(f"Hafta 3 olcum dosyasi yok: {args.measurements}\n"
              "  beklenen: kayitNN_<surum>_<yuk>_iz_<acik|kapali>_meta.json + .csv\n"
              "  uretmek icin: python analysis/queue_experiment.py --port COMx  ya da  interface/index.html")
        return 1

    # Ozet tablosu: queue_experiment.py --ozet ile ayni dosya ve bicim.
    qe.MEAS_DIR = args.measurements
    qe.SUMMARY_CSV = args.measurements / "queue_summary.csv"
    qe.write_summary()

    write_report(runs, args.rapor)
    # Konsol ozeti ASCII: Windows konsolu (cp1252/cp857) Turkce karakter ve ok basamayabilir.
    for r in runs:
        o, k = r.ozet, r.kosu
        print(f"{k['kayit']}{'' if k['tekrar'] == 1 else ' t' + str(k['tekrar'])} {qe.vname(k['variant'])}/{k['load']:<6} "
              f"iz {'acik  ' if k['trace'] else 'kapali'} kabul {o['accepted']:2} kayip {o['failed']:2} "
              f"kuyruk tepe {o['queue_max']:2}/{k['capacity']:<2} uretim->alma maks {o['g2r_max_us'] / 1000:8.2f} ms  "
              f"uretici gonderimde {o['producer_send_total_ms']:7.1f} ms  "
              f"{'kontroller tamam' if r.checks_ok else 'KONTROL HATASI'}")

    args.plots.mkdir(parents=True, exist_ok=True)
    made = [name for name, ok in [
        ("zaman_cizelgesi_yogun.png", plot_timelines(runs, args.plots)),
        ("kabul_kayip.png", plot_accept_loss(runs, args.plots)),
        ("doluluk_gecikme.png", plot_depth_latency(runs, args.plots)),
    ] if ok]

    bad = [r.label for r in runs if not r.checks_ok]
    missing = [k for k in qe.RECORDS if k not in {r.kosu["kayit"] for r in runs}]
    print(f"\nyazildi: {qe.SUMMARY_CSV}, {args.rapor}")
    print(f"grafikler ({args.plots}): {', '.join(made)}")
    if missing:
        print(f"henuz alinmayan kayit: {', '.join(missing)}")
    if bad:
        print(f"UYARI: kontrolleri gecmeyen kosu: {', '.join(bad)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

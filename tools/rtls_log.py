#!/usr/bin/env python3
"""Ferramentas de bancada do RTLS (docs/07-bancada.md).

Subcomandos:
  capture  lê a UART e grava "R,..." em <out>, "L,..." em <out>_pos.csv e
           "P,..." (gateway) em <out>_gw.csv
  stats    estatísticas por âncora (taxa de sucesso, média, desvio, potência)
  pos      estatísticas das posições do EKF (dispersão, RMSE, CEP50/95)
  gw       entrega das posições no gateway (lacunas de round, potência)
  calib    viés e ajuste do atraso de antena com um par de placas iguais
  calib3   ajuste individual do atraso de antena com três placas (APS014)

Usa só a biblioteca padrão e o pyserial (já instalado no venv do Zephyr).
"""

import argparse
import csv
import re
import statistics
import sys
import time
from collections import defaultdict
from pathlib import Path

# A coluna raw_mm (antes da correção de viés por RSL) é opcional: firmwares
# antigos não a imprimem.
LINE_RE = re.compile(r"R,(\d+),(\d+),([0-9a-fA-F]{4}),(-?\d+),(-?\d+),(-?\d+)(?:,(-?\d+))?")
FIELDS = ["round", "t_ms", "anchor", "dist_mm", "rx_power_cdbm", "fp_power_cdbm", "raw_mm"]

# Posição do EKF: L,round,t_ms,x,y,z,sx,sy,sz,n_ranges,flags (mm).
POS_RE = re.compile(
    r"(?<![A-Za-z])L,(\d+),(\d+),(-?\d+),(-?\d+),(-?\d+),(\d+),(\d+),(\d+),(\d+),(\d+)"
)
POS_FIELDS = ["round", "t_ms", "x_mm", "y_mm", "z_mm", "sx_mm", "sy_mm", "sz_mm", "n_ranges",
              "flags"]
# POSITION recebido no gateway: P,tag,round,t_ms,x,y,z,sx,sy,sz,n,flags,rx_cdbm,fp_cdbm,gw_ms.
GW_RE = re.compile(
    r"(?<![A-Za-z])P,([0-9a-fA-F]{4}),(\d+),(\d+),(-?\d+),(-?\d+),(-?\d+),(\d+),(\d+),(\d+),"
    r"(\d+),(\d+),(-?\d+),(-?\d+),(\d+)"
)
GW_FIELDS = ["tag", "round", "t_ms", "x_mm", "y_mm", "z_mm", "sx_mm", "sy_mm", "sz_mm",
             "n_ranges", "flags", "rx_power_cdbm", "fp_power_cdbm", "gw_ms"]
# Linha do log do Zephyr quando o buffer do log diferido enche: o CSV
# (printk via CONFIG_LOG_PRINTK) perdeu linhas.
DROPPED_RE = re.compile(r"--- (\d+) messages dropped ---")
# Colunas que ficam como texto (IDs em hexadecimal).
TEXT_FIELDS = ("anchor", "tag")
# RTLS_POS_FLAG_* (include/rtls/frames.h)
FLAG_INIT = 1 << 1
FLAG_OUTLIER = 1 << 2
FLAG_3D = 1 << 3

# Metros por tick do DW1000: c_ar / (128 * 499,2 MHz) ~= 4,69 mm.
M_PER_TICK = 299702547.0 / (128 * 499.2e6)
DEFAULT_ANT_DLY = 16436


def parse_line(line):
    match = LINE_RE.search(line)
    if not match:
        return None
    rnd, t_ms, anchor, dist, rx_pwr, fp_pwr, raw = match.groups()
    return {
        "round": int(rnd),
        "t_ms": int(t_ms),
        "anchor": anchor.lower(),
        "dist_mm": int(dist),
        "rx_power_cdbm": int(rx_pwr),
        "fp_power_cdbm": int(fp_pwr),
        "raw_mm": int(raw) if raw is not None else None,
    }


def parse_pos(line):
    match = POS_RE.search(line)
    if not match:
        return None
    return dict(zip(POS_FIELDS, (int(v) for v in match.groups())))


def parse_gw(line):
    match = GW_RE.search(line)
    if not match:
        return None
    values = match.groups()
    row = dict(zip(GW_FIELDS[1:], (int(v) for v in values[1:])))
    row["tag"] = values[0].lower()
    return row


def sibling_path(out, suffix):
    out = Path(out)
    return out.with_name(f"{out.stem}{suffix}{out.suffix or '.csv'}")


def pos_path(out):
    return sibling_path(out, "_pos")


def load(path):
    with open(path, newline="") as f:
        return [
            {
                k: (v if k in TEXT_FIELDS else int(v) if v not in (None, "") else None)
                for k, v in row.items()
            }
            for row in csv.DictReader(f)
        ]


def by_anchor(rows):
    groups = defaultdict(list)
    for row in rows:
        groups[row["anchor"]].append(row)
    return dict(sorted(groups.items()))


def single_anchor(rows, anchor, path):
    groups = by_anchor(rows)
    if anchor:
        anchor = anchor.lower().removeprefix("0x")
        if anchor not in groups:
            sys.exit(f"{path}: âncora {anchor} não encontrada ({', '.join(groups)})")
        return groups[anchor]
    if len(groups) != 1:
        sys.exit(f"{path}: várias âncoras ({', '.join(groups)}); use --anchor")
    return next(iter(groups.values()))


def bias_ticks(rows, true_m):
    mean_m = statistics.fmean(r["dist_mm"] for r in rows) / 1000.0
    return (mean_m - true_m) / M_PER_TICK, mean_m


def cmd_capture(args):
    try:
        import serial
    except ImportError:
        sys.exit("pyserial não encontrado: ative o venv (scripts/env.*) ou pip install pyserial")

    # Cada tipo de linha vai para o seu arquivo, criado só quando aparece a
    # primeira linha dele: a tag gera R e L; o gateway, P.
    sinks = [
        {"parse": parse_line, "path": Path(args.out), "fields": FIELDS, "what": "distâncias"},
        {"parse": parse_pos, "path": pos_path(args.out), "fields": POS_FIELDS,
         "what": "posições da tag"},
        {"parse": parse_gw, "path": sibling_path(args.out, "_gw"), "fields": GW_FIELDS,
         "what": "posições no gateway"},
    ]
    for sink in sinks:
        sink.update(file=None, writer=None, count=0)
    dropped = 0

    deadline = time.monotonic() + args.seconds if args.seconds else None
    with serial.Serial(args.port, args.baud, timeout=1) as port:
        print(f"Capturando {args.port} -> {args.out} (Ctrl+C para parar)")
        try:
            while deadline is None or time.monotonic() < deadline:
                line = port.readline().decode(errors="replace")
                if args.echo and line:
                    print(line, end="")
                drop = DROPPED_RE.search(line)
                if drop:
                    dropped += int(drop.group(1))
                    continue
                for sink in sinks:
                    row = sink["parse"](line)
                    if not row:
                        continue
                    if sink["writer"] is None:
                        sink["file"] = open(sink["path"], "w", newline="")
                        sink["writer"] = csv.DictWriter(sink["file"], fieldnames=sink["fields"])
                        sink["writer"].writeheader()
                    sink["writer"].writerow(row)
                    sink["count"] += 1
                    break
        except KeyboardInterrupt:
            pass
        finally:
            for sink in sinks:
                if sink["file"]:
                    sink["file"].close()

    written = [sink for sink in sinks if sink["count"]]
    for sink in written:
        print(f"{sink['count']} {sink['what']} gravadas em {sink['path']}")
    if not written:
        print("nenhuma linha R, L ou P recebida: confira a porta e o firmware (--echo mostra tudo)")
    if dropped:
        print(f"ATENÇÃO: o firmware descartou {dropped} mensagens do log durante a captura; "
              "o CSV está incompleto (aumente CONFIG_LOG_BUFFER_SIZE)")


def cmd_stats(args):
    rows = load(args.csv)
    if not rows:
        sys.exit("CSV vazio")

    rounds = len({r["t_ms"] for r in rows})
    span_s = (max(r["t_ms"] for r in rows) - min(r["t_ms"] for r in rows)) / 1000.0
    rate = (rounds - 1) / span_s if span_s > 0 else 0.0
    print(f"{rounds} rodadas com ao menos uma distância, {span_s:.1f} s, {rate:.2f} Hz")
    print(
        f"{'âncora':>7} {'n':>6} {'sucesso':>8} {'média mm':>9} {'bruta mm':>9} "
        f"{'desvio':>7} {'mín':>6} {'máx':>6} {'RX dBm':>7} {'1º cam.':>7}"
    )
    for anchor, group in by_anchor(rows).items():
        dists = [r["dist_mm"] for r in group]
        raws = [r["raw_mm"] for r in group if r.get("raw_mm") is not None]
        raw_mean = f"{statistics.fmean(raws):>9.1f}" if raws else f"{'-':>9}"
        std = statistics.stdev(dists) if len(dists) > 1 else 0.0
        rx = statistics.fmean(r["rx_power_cdbm"] for r in group) / 100.0
        fp = statistics.fmean(r["fp_power_cdbm"] for r in group) / 100.0
        print(
            f"{anchor:>7} {len(group):>6} {100.0 * len(group) / rounds:>7.1f}% "
            f"{statistics.fmean(dists):>9.1f} {raw_mean} {std:>7.1f} {min(dists):>6} {max(dists):>6} "
            f"{rx:>7.1f} {fp:>7.1f}"
        )
        if args.true_distance is not None:
            ticks, mean_m = bias_ticks(group, args.true_distance)
            print(f"{'':>7} viés {1000 * (mean_m - args.true_distance):+.1f} mm "
                  f"({ticks:+.1f} ticks)")


def percentile(sorted_values, p):
    if not sorted_values:
        return float("nan")
    k = (len(sorted_values) - 1) * p / 100.0
    lo = int(k)
    hi = min(lo + 1, len(sorted_values) - 1)
    return sorted_values[lo] + (sorted_values[hi] - sorted_values[lo]) * (k - lo)


def select_tag(rows, tag, path):
    """No CSV do gateway (coluna tag), fica só com uma tag."""
    if not rows or "tag" not in rows[0]:
        return rows
    tags = sorted({r["tag"] for r in rows})
    if tag:
        tag = tag.lower().removeprefix("0x")
        if tag not in tags:
            sys.exit(f"{path}: tag {tag} não encontrada ({', '.join(tags)})")
        return [r for r in rows if r["tag"] == tag]
    if len(tags) != 1:
        sys.exit(f"{path}: várias tags ({', '.join(tags)}); use --tag")
    return rows


def cmd_pos(args):
    rows = select_tag(load(args.csv), args.tag, args.csv)[args.skip:]
    if not rows:
        sys.exit("CSV vazio")

    span_s = (rows[-1]["t_ms"] - rows[0]["t_ms"]) / 1000.0
    rate = (len(rows) - 1) / span_s if span_s > 0 else 0.0
    n_init = sum(1 for r in rows if r["flags"] & FLAG_INIT)
    n_out = sum(1 for r in rows if r["flags"] & FLAG_OUTLIER)
    is_3d = all(r["flags"] & FLAG_3D for r in rows)
    print(f"{len(rows)} posições, {span_s:.1f} s, {rate:.2f} Hz, modo {'3D' if is_3d else '2D'}; "
          f"INIT {n_init}, com OUTLIER {n_out}, "
          f"distâncias por posição {statistics.fmean(r['n_ranges'] for r in rows):.2f}")

    print(f"{'eixo':>5} {'média mm':>9} {'desvio':>7} {'mín':>7} {'máx':>7} {'sig.EKF':>7}")
    for axis in ("x", "y", "z"):
        vals = [r[f"{axis}_mm"] for r in rows]
        sig = statistics.fmean(r[f"s{axis}_mm"] for r in rows)
        std = statistics.stdev(vals) if len(vals) > 1 else 0.0
        print(f"{axis:>5} {statistics.fmean(vals):>9.1f} {std:>7.1f} {min(vals):>7} "
              f"{max(vals):>7} {sig:>7.1f}")

    if args.true_pos:
        truth = [1000.0 * float(v) for v in args.true_pos.split(",")]
        if len(truth) not in (2, 3):
            sys.exit("--true-pos espera x,y ou x,y,z (m)")
        horiz = sorted(
            ((r["x_mm"] - truth[0]) ** 2 + (r["y_mm"] - truth[1]) ** 2) ** 0.5 for r in rows
        )
        rmse_2d = (statistics.fmean(e * e for e in horiz)) ** 0.5
        bias = [statistics.fmean(r[f"{a}_mm"] for r in rows) - t
                for a, t in zip(("x", "y", "z"), truth)]
        print("viés " + "  ".join(f"{a} {b:+.1f} mm" for a, b in zip(("x", "y", "z"), bias)))
        print(f"RMSE 2D {rmse_2d:.1f} mm, CEP50 {percentile(horiz, 50):.1f} mm, "
              f"CEP95 {percentile(horiz, 95):.1f} mm")
        if len(truth) == 3 and is_3d:
            rmse_3d = statistics.fmean(
                (r["x_mm"] - truth[0]) ** 2 + (r["y_mm"] - truth[1]) ** 2
                + (r["z_mm"] - truth[2]) ** 2 for r in rows
            ) ** 0.5
            print(f"RMSE 3D {rmse_3d:.1f} mm")


def round_gaps(rows):
    """Mesma regra de rtls_round_tracker_update() (lib/proto/frames.c).

    Devolve (rodadas sem POSITION, repetidos, reinícios da tag).
    """
    lost = dup = restarts = 0
    last = None
    for r in rows:
        if last is not None:
            if r["t_ms"] == last["t_ms"]:
                dup += 1
                continue
            if r["t_ms"] > last["t_ms"]:
                lost += (r["round"] - last["round"] - 1) % 256
            else:
                restarts += 1
        last = r
    return lost, dup, restarts


def cmd_gw(args):
    rows = load(args.csv)
    if not rows:
        sys.exit("CSV vazio")

    sent = None
    if args.sent:
        sent = {r["t_ms"] for r in load(args.sent)}

    print(f"{'tag':>5} {'n':>6} {'s':>7} {'Hz':>6} {'lacunas':>8} {'recebidas':>10} "
          f"{'maior int.':>10} {'RX dBm':>7} {'1º cam.':>7}")
    for tag in sorted({r["tag"] for r in rows}):
        group = [r for r in rows if r["tag"] == tag]
        lost, dup, restarts = round_gaps(group)
        n = len(group) - dup
        span_s = (group[-1]["gw_ms"] - group[0]["gw_ms"]) / 1000.0
        rate = (n - 1) / span_s if span_s > 0 else 0.0
        gaps = [b["gw_ms"] - a["gw_ms"] for a, b in zip(group, group[1:])]
        rx = statistics.fmean(r["rx_power_cdbm"] for r in group) / 100.0
        fp = statistics.fmean(r["fp_power_cdbm"] for r in group) / 100.0
        print(f"{tag:>5} {n:>6} {span_s:>7.1f} {rate:>6.2f} {lost:>8} "
              f"{100.0 * n / (n + lost):>9.2f}% {max(gaps, default=0):>8} ms "
              f"{rx:>7.1f} {fp:>7.1f}")
        if dup or restarts:
            print(f"{'':>5} {dup} repetidos, {restarts} reinícios da tag")

    if sent is not None:
        tags = sorted({r["tag"] for r in rows})
        if len(tags) != 1:
            sys.exit("--sent compara com uma tag só: capture o gateway com uma tag ligada")
        got = {r["t_ms"] for r in rows}
        # só conta o trecho em que as duas capturas se sobrepõem
        lo, hi = min(got), max(got)
        window = {t for t in sent if lo <= t <= hi}
        delivered = len(window & got)
        print(f"enviadas pela tag no trecho comum: {len(window)}, recebidas: {delivered} "
              f"({100.0 * delivered / len(window) if window else 0.0:.2f}%)")


def print_delays(label, current_tx, current_rx, delta_ticks):
    half = delta_ticks / 2.0
    tx = round(current_tx + half)
    rx = round(current_rx + half)
    print(f"{label}: delta {delta_ticks:+.1f} ticks -> "
          f"CONFIG_RTLS_DW1000_TX_ANT_DLY={tx}  CONFIG_RTLS_DW1000_RX_ANT_DLY={rx}")


def cmd_calib(args):
    rows = single_anchor(load(args.csv), args.anchor, args.csv)
    ticks, mean_m = bias_ticks(rows, args.true_distance)
    print(f"{len(rows)} medidas: média {mean_m:.4f} m, real {args.true_distance:.4f} m, "
          f"viés {1000 * (mean_m - args.true_distance):+.1f} mm = {ticks:+.1f} ticks")
    print("Supondo as duas placas iguais, cada uma recebe o ajuste inteiro:")
    print_delays("ambas", args.current_tx, args.current_rx, ticks)


def cmd_calib3(args):
    bias = {}
    for pair, path in (("ab", args.ab), ("ac", args.ac), ("bc", args.bc)):
        rows = single_anchor(load(path), None, path)
        bias[pair], mean_m = bias_ticks(rows, args.true_distance)
        print(f"par {pair.upper()}: {len(rows)} medidas, média {mean_m:.4f} m, "
              f"viés {bias[pair]:+.1f} ticks")

    deltas = {
        "A": bias["ab"] + bias["ac"] - bias["bc"],
        "B": bias["ab"] + bias["bc"] - bias["ac"],
        "C": bias["ac"] + bias["bc"] - bias["ab"],
    }
    for board, delta in deltas.items():
        print_delays(f"placa {board}", args.current_tx, args.current_rx, delta)


def main():
    # Consoles/pipes do Windows em cp1252: acentos passam, o resto vira "?".
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")

    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("capture", help="grava as distâncias da UART da tag em CSV")
    p.add_argument("--port", required=True, help="COMx ou /dev/ttyACMx")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--out", required=True)
    p.add_argument("--seconds", type=float, help="duração (padrão: até Ctrl+C)")
    p.add_argument("--echo", action="store_true", help="mostra tudo o que chega na UART")
    p.set_defaults(func=cmd_capture)

    p = sub.add_parser("stats", help="estatísticas por âncora")
    p.add_argument("csv")
    p.add_argument("--true-distance", type=float, help="distância real (m) para o viés")
    p.set_defaults(func=cmd_stats)

    p = sub.add_parser("pos", help="estatísticas das posições (<captura>_pos.csv ou _gw.csv)")
    p.add_argument("csv")
    p.add_argument("--true-pos", help="posição real x,y[,z] em metros, para viés/RMSE/CEP")
    p.add_argument("--skip", type=int, default=0, help="descarta as primeiras N posições")
    p.add_argument("--tag", help="ID da tag, se o CSV do gateway tiver várias")
    p.set_defaults(func=cmd_pos)

    p = sub.add_parser("gw", help="entrega das posições no gateway (<captura>_gw.csv)")
    p.add_argument("csv")
    p.add_argument("--sent", help="<captura>_pos.csv da tag, gravado ao mesmo tempo: "
                   "compara enviadas x recebidas pelo t_ms")
    p.set_defaults(func=cmd_gw)

    for name, func, helptext in (
        ("calib", cmd_calib, "ajuste com um par de placas iguais"),
        ("calib3", cmd_calib3, "ajuste individual com três placas"),
    ):
        p = sub.add_parser(name, help=helptext)
        p.add_argument("--true-distance", type=float, required=True, help="distância real (m)")
        p.add_argument("--current-tx", type=int, default=DEFAULT_ANT_DLY)
        p.add_argument("--current-rx", type=int, default=DEFAULT_ANT_DLY)
        if name == "calib":
            p.add_argument("csv")
            p.add_argument("--anchor", help="ID da âncora se o CSV tiver várias")
        else:
            p.add_argument("--ab", required=True, help="CSV do par A-B")
            p.add_argument("--ac", required=True, help="CSV do par A-C")
            p.add_argument("--bc", required=True, help="CSV do par B-C")
        p.set_defaults(func=func)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()

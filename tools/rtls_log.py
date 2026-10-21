#!/usr/bin/env python3
"""Ferramentas de bancada do RTLS (docs/07-bancada.md).

Subcomandos:
  capture  lê a UART da tag e grava as linhas "R,..." em CSV
  stats    estatísticas por âncora (taxa de sucesso, média, desvio, potência)
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

LINE_RE = re.compile(r"R,(\d+),(\d+),([0-9a-fA-F]{4}),(-?\d+),(-?\d+),(-?\d+)")
FIELDS = ["round", "t_ms", "anchor", "dist_mm", "rx_power_cdbm", "fp_power_cdbm"]

# Metros por tick do DW1000: c_ar / (128 * 499,2 MHz) ~= 4,69 mm.
M_PER_TICK = 299702547.0 / (128 * 499.2e6)
DEFAULT_ANT_DLY = 16436


def parse_line(line):
    match = LINE_RE.search(line)
    if not match:
        return None
    rnd, t_ms, anchor, dist, rx_pwr, fp_pwr = match.groups()
    return {
        "round": int(rnd),
        "t_ms": int(t_ms),
        "anchor": anchor.lower(),
        "dist_mm": int(dist),
        "rx_power_cdbm": int(rx_pwr),
        "fp_power_cdbm": int(fp_pwr),
    }


def load(path):
    with open(path, newline="") as f:
        return [
            {k: (v if k == "anchor" else int(v)) for k, v in row.items()}
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

    count = 0
    deadline = time.monotonic() + args.seconds if args.seconds else None
    with serial.Serial(args.port, args.baud, timeout=1) as port, open(
        args.out, "w", newline=""
    ) as f:
        writer = csv.DictWriter(f, fieldnames=FIELDS)
        writer.writeheader()
        print(f"Capturando {args.port} -> {args.out} (Ctrl+C para parar)")
        try:
            while deadline is None or time.monotonic() < deadline:
                line = port.readline().decode(errors="replace")
                if args.echo and line:
                    print(line, end="")
                row = parse_line(line)
                if row:
                    writer.writerow(row)
                    count += 1
        except KeyboardInterrupt:
            pass
    print(f"{count} distâncias gravadas em {args.out}")


def cmd_stats(args):
    rows = load(args.csv)
    if not rows:
        sys.exit("CSV vazio")

    rounds = len({r["t_ms"] for r in rows})
    span_s = (max(r["t_ms"] for r in rows) - min(r["t_ms"] for r in rows)) / 1000.0
    rate = (rounds - 1) / span_s if span_s > 0 else 0.0
    print(f"{rounds} rodadas com ao menos uma distância, {span_s:.1f} s, {rate:.2f} Hz")
    print(
        f"{'âncora':>7} {'n':>6} {'sucesso':>8} {'média mm':>9} {'desvio':>7} "
        f"{'mín':>6} {'máx':>6} {'RX dBm':>7} {'1º cam.':>7}"
    )
    for anchor, group in by_anchor(rows).items():
        dists = [r["dist_mm"] for r in group]
        std = statistics.stdev(dists) if len(dists) > 1 else 0.0
        rx = statistics.fmean(r["rx_power_cdbm"] for r in group) / 100.0
        fp = statistics.fmean(r["fp_power_cdbm"] for r in group) / 100.0
        print(
            f"{anchor:>7} {len(group):>6} {100.0 * len(group) / rounds:>7.1f}% "
            f"{statistics.fmean(dists):>9.1f} {std:>7.1f} {min(dists):>6} {max(dists):>6} "
            f"{rx:>7.1f} {fp:>7.1f}"
        )
        if args.true_distance is not None:
            ticks, mean_m = bias_ticks(group, args.true_distance)
            print(f"{'':>7} viés {1000 * (mean_m - args.true_distance):+.1f} mm "
                  f"({ticks:+.1f} ticks)")


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

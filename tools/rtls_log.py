#!/usr/bin/env python3
"""Ferramentas de bancada do RTLS (docs/07-bancada.md).

Subcomandos:
  capture  lê a UART e grava "R,..." em <out>, "L,..." em <out>_pos.csv e
           "P,..." (gateway) em <out>_gw.csv
  stats    estatísticas por âncora (taxa de sucesso, média, desvio, potência)
  pos      estatísticas das posições do EKF (dispersão, RMSE, CEP50/95)
  gw       entrega das posições no gateway (lacunas de round, potência)
  replay   reprocessa as distâncias no mesmo LS/EKF do firmware (T4.8, E7)
  calib    viés e ajuste do atraso de antena com um par de placas iguais
  calib3   ajuste individual do atraso de antena com três placas (APS014)

Usa só a biblioteca padrão e o pyserial (já instalado no venv do Zephyr).
"""

import argparse
import csv
import math
import re
import statistics
import sys
import time
from collections import defaultdict
from pathlib import Path

# A coluna raw_mm (antes da correção de viés por RSL) é opcional: firmwares
# antigos não a imprimem.
# A coluna resp_fp_cdbm (1º caminho do RESP na tag, M8) também é opcional; com
# ela, rx_power_cdbm - resp_fp_cdbm é o indicador de NLOS do mesmo quadro.
LINE_RE = re.compile(
    r"R,(\d+),(\d+),([0-9a-fA-F]{4}),(-?\d+),(-?\d+),(-?\d+)(?:,(-?\d+))?(?:,(-?\d+))?"
)
FIELDS = ["round", "t_ms", "anchor", "dist_mm", "rx_power_cdbm", "fp_power_cdbm", "raw_mm",
          "resp_fp_cdbm"]
# Indicador acima do qual a distância é suspeita de NLOS (dB): regra do DW1000.
NLOS_SUSPECT_DB = 6.0
# Sequências VT100 que o shell (M8) insere ao redesenhar o prompt.
ANSI_RE = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")

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
    rnd, t_ms, anchor, dist, rx_pwr, fp_pwr, raw, resp_fp = match.groups()
    return {
        "round": int(rnd),
        "t_ms": int(t_ms),
        "anchor": anchor.lower(),
        "dist_mm": int(dist),
        "rx_power_cdbm": int(rx_pwr),
        "fp_power_cdbm": int(fp_pwr),
        "raw_mm": int(raw) if raw is not None else None,
        "resp_fp_cdbm": int(resp_fp) if resp_fp is not None else None,
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
                line = ANSI_RE.sub("", line)
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
        f"{'desvio':>7} {'mín':>6} {'máx':>6} {'RX dBm':>7} {'1º cam.':>7} {'NLOS dB':>8} "
        f"{'>6 dB':>6}"
    )
    for anchor, group in by_anchor(rows).items():
        dists = [r["dist_mm"] for r in group]
        raws = [r["raw_mm"] for r in group if r.get("raw_mm") is not None]
        raw_mean = f"{statistics.fmean(raws):>9.1f}" if raws else f"{'-':>9}"
        std = statistics.stdev(dists) if len(dists) > 1 else 0.0
        rx = statistics.fmean(r["rx_power_cdbm"] for r in group) / 100.0
        fp = statistics.fmean(r["fp_power_cdbm"] for r in group) / 100.0
        # P_RX - P_FP do mesmo RESP, na tag (só com a coluna resp_fp_cdbm)
        nlos = [(r["rx_power_cdbm"] - r["resp_fp_cdbm"]) / 100.0 for r in group
                if r.get("resp_fp_cdbm") is not None]
        if nlos:
            suspect = 100.0 * sum(1 for v in nlos if v > NLOS_SUSPECT_DB) / len(nlos)
            nlos_txt = f"{statistics.fmean(nlos):>8.1f} {suspect:>5.1f}%"
        else:
            nlos_txt = f"{'-':>8} {'-':>6}"
        print(
            f"{anchor:>7} {len(group):>6} {100.0 * len(group) / rounds:>7.1f}% "
            f"{statistics.fmean(dists):>9.1f} {raw_mean} {std:>7.1f} {min(dists):>6} {max(dists):>6} "
            f"{rx:>7.1f} {fp:>7.1f} {nlos_txt}"
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


def load_anchors(path):
    """CSV anchor,x_mm,y_mm,z_mm -> {id: (x, y, z) em metros}."""
    anchors = {}
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            anchor = row["anchor"].strip().lower().removeprefix("0x")
            anchors[anchor] = tuple(int(row[k]) / 1000.0 for k in ("x_mm", "y_mm", "z_mm"))
    if not anchors:
        sys.exit(f"{path}: nenhuma âncora (cabeçalho anchor,x_mm,y_mm,z_mm)")
    return anchors


def cmd_replay(args):
    import rtls_loc

    anchors = load_anchors(args.anchors)
    rows = load(args.csv)
    if not rows:
        sys.exit("CSV vazio")

    cfg = rtls_loc.Cfg(dim=3 if args.mode == "3d" else 2, tag_height_m=args.tag_height,
                       range_std_m=args.range_std, accel_std_mps2=args.accel_std,
                       gate_chi2=args.gate, nlos_weighting=args.nlos_weight)
    ekf = rtls_loc.Ekf(cfg)

    # Uma rodada = as linhas R com o mesmo (round, t_ms), na ordem da captura.
    rounds = []
    for r in rows:
        key = (r["round"], r["t_ms"])
        if not rounds or rounds[-1][0] != key:
            rounds.append((key, []))
        rounds[-1][1].append(r)

    unknown = set()
    out_rows = []
    for (rnd, t_ms), group in rounds:
        ranges = []
        for r in group:
            if r["anchor"] not in anchors:
                unknown.add(r["anchor"])
                continue
            ranges.append(rtls_loc.Range(anchors[r["anchor"]], r["dist_mm"] / 1000.0,
                                         r["rx_power_cdbm"], r.get("resp_fp_cdbm") or 0))

        if args.method == "ekf":
            res = ekf.update(t_ms, ranges)
        else:
            sol = rtls_loc.solve(cfg, ranges)
            res = None
            if sol is not None:
                pos, cov, s2 = sol
                var0 = max(cfg.range_std_m ** 2, s2)
                std = [math.sqrt(max(var0 * cov[i][i], 0.0)) for i in range(cfg.dim)]
                std += [0.0] * (3 - len(std))
                res = (pos, std, len(ranges),
                       rtls_loc.FLAG_VALID | (rtls_loc.FLAG_3D if cfg.dim == 3 else 0))
        if res is None:
            continue
        pos, std, n_used, flags = res
        out_rows.append({
            "round": rnd, "t_ms": t_ms,
            "x_mm": round(pos[0] * 1000), "y_mm": round(pos[1] * 1000),
            "z_mm": round(pos[2] * 1000),
            "sx_mm": min(round(std[0] * 1000), 65535), "sy_mm": min(round(std[1] * 1000), 65535),
            "sz_mm": min(round(std[2] * 1000), 65535),
            "n_ranges": n_used, "flags": flags,
        })

    with open(args.out, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=POS_FIELDS)
        writer.writeheader()
        writer.writerows(out_rows)

    print(f"{len(rounds)} rodadas, {len(out_rows)} posições ({args.method}, {args.mode}) "
          f"gravadas em {args.out}")
    if args.method == "ekf":
        st = ekf.stats
        print(f"EKF: {st['accepted']} aceitas, {st['rejected']} rejeitadas, {st['resets']} "
              f"reinícios, {st['nlos_weighted']} ponderadas por NLOS")
    if unknown:
        print(f"ATENÇÃO: âncoras sem posição em {args.anchors} (ignoradas): "
              f"{', '.join(sorted(unknown))}")

    if args.compare:
        tag = {r["t_ms"]: r for r in load(args.compare)}
        diffs = {a: [] for a in ("x", "y", "z")}
        flag_diff = 0
        for r in out_rows:
            t = tag.get(r["t_ms"])
            if t is None:
                continue
            for a in diffs:
                diffs[a].append(abs(r[f"{a}_mm"] - t[f"{a}_mm"]))
            flag_diff += (r["flags"] != t["flags"])
        n = len(diffs["x"])
        if n == 0:
            print(f"{args.compare}: nenhuma posição com o mesmo t_ms")
            return
        print(f"comparação com {args.compare}: {n} posições em comum, {flag_diff} com flags "
              "diferentes")
        for a, v in diffs.items():
            print(f"  {a}: |Δ| médio {statistics.fmean(v):.1f} mm, máximo {max(v)} mm")


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
        # Cada par pode ter a sua distância medida (--d-ab etc.); senão, a comum.
        true_m = getattr(args, f"d_{pair}") or args.true_distance
        if true_m is None:
            sys.exit(f"par {pair.upper()}: falta a distância real "
                     f"(--true-distance ou --d-{pair})")
        rows = single_anchor(load(path), None, path)
        bias[pair], mean_m = bias_ticks(rows, true_m)
        print(f"par {pair.upper()}: {len(rows)} medidas, média {mean_m:.4f} m, "
              f"real {true_m:.4f} m, viés {bias[pair]:+.1f} ticks")

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

    p = sub.add_parser("replay", help="reprocessa as distâncias no LS/EKF do firmware")
    p.add_argument("csv", help="CSV de distâncias (linhas R)")
    p.add_argument("--anchors", required=True, help="CSV anchor,x_mm,y_mm,z_mm das âncoras")
    p.add_argument("--out", required=True, help="posições, no formato do <captura>_pos.csv")
    p.add_argument("--method", choices=("ekf", "ls"), default="ekf",
                   help="EKF completo ou multilateração por rodada (E7)")
    p.add_argument("--mode", choices=("2d", "3d"), default="2d")
    p.add_argument("--tag-height", type=float, default=1.0, help="z fixo da tag em 2D (m)")
    p.add_argument("--range-std", type=float, default=0.10, help="sigma_r (m)")
    p.add_argument("--accel-std", type=float, default=1.0, help="sigma_a (m/s²)")
    p.add_argument("--gate", type=float, default=6.63, help="limiar do gating (chi²)")
    p.add_argument("--nlos-weight", action="store_true", help="ponderação por NLOS (M8)")
    p.add_argument("--compare", help="_pos.csv da tag (linhas L) para conferir o firmware")
    p.set_defaults(func=cmd_replay)

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
        p.add_argument("--true-distance", type=float, required=(name == "calib"),
                       help="distância real (m)" + ("" if name == "calib" else
                                                    ", comum aos pares sem --d-xx"))
        p.add_argument("--current-tx", type=int, default=DEFAULT_ANT_DLY)
        p.add_argument("--current-rx", type=int, default=DEFAULT_ANT_DLY)
        if name == "calib":
            p.add_argument("csv")
            p.add_argument("--anchor", help="ID da âncora se o CSV tiver várias")
        else:
            for pair in ("ab", "ac", "bc"):
                p.add_argument(f"--{pair}", required=True,
                               help=f"CSV do par {pair[0].upper()}-{pair[1].upper()}")
                p.add_argument(f"--d-{pair}", type=float,
                               help=f"distância real do par {pair.upper()} (m)")
        p.set_defaults(func=func)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()

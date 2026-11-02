"""Port em Python de lib/loc/loc.c (multilateração LS + Gauss-Newton e EKF).

Serve para reprocessar no PC as distâncias de uma captura (T4.8) e comparar
métodos e sintonias (E7) sem regravar a tag. Mesmas constantes, regras de
reinício e flags do firmware; usa float64, então difere do firmware (float32)
só no arredondamento. Especificação: docs/04-algoritmos.md, seções 4 e 5.

Só biblioteca padrão.
"""

import math

# Constantes de lib/loc/loc.c e loc_mat.c
GN_ITERS = 3
GN_STOP_M = 1e-4
MIN_RANGE_M = 1e-3
TIMEOUT_MS = 1000
MAX_REJECT_STREAK = 5
MAX_POS_VAR_M2 = 100.0
INIT_VEL_STD_MPS = 1.0
CHOL_REL_EPS = 1e-6

# RTLS_POS_FLAG_* (include/rtls/frames.h)
FLAG_VALID = 1 << 0
FLAG_INIT = 1 << 1
FLAG_OUTLIER = 1 << 2
FLAG_3D = 1 << 3


class Cfg:
    """struct rtls_loc_cfg (metros, m/s², dB)."""

    def __init__(self, dim=2, tag_height_m=1.0, range_std_m=0.10, accel_std_mps2=1.0,
                 gate_chi2=6.63, nlos_weighting=False, nlos_los_db=6.0, nlos_nlos_db=10.0,
                 nlos_var_gain=9.0):
        self.dim = dim
        self.tag_height_m = tag_height_m
        self.range_std_m = range_std_m
        self.accel_std_mps2 = accel_std_mps2
        self.gate_chi2 = gate_chi2
        self.nlos_weighting = nlos_weighting
        self.nlos_los_db = nlos_los_db
        self.nlos_nlos_db = nlos_nlos_db
        self.nlos_var_gain = nlos_var_gain


class Range:
    """struct rtls_range: posição da âncora (m), distância (m) e potências (cdBm)."""

    def __init__(self, anchor, distance_m, rx_power_cdbm=0, resp_fp_power_cdbm=0):
        self.anchor = anchor
        self.distance_m = distance_m
        self.rx_power_cdbm = rx_power_cdbm
        self.resp_fp_power_cdbm = resp_fp_power_cdbm


# ------------------------------------------------------------ matrizes

def zeros(r, c):
    return [[0.0] * c for _ in range(r)]


def identity(n):
    m = zeros(n, n)
    for i in range(n):
        m[i][i] = 1.0
    return m


def mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(len(b))) for j in range(len(b[0]))]
            for i in range(len(a))]


def mul_bt(a, b):
    return [[sum(a[i][k] * b[j][k] for k in range(len(a[0]))) for j in range(len(b))]
            for i in range(len(a))]


def symmetrize(m):
    n = len(m)
    for i in range(n):
        for j in range(i + 1, n):
            s = 0.5 * (m[i][j] + m[j][i])
            m[i][j] = m[j][i] = s


def cholesky(a):
    """Fator l (a = l lᵀ) ou None se não for positiva definida (como em loc_mat.c)."""
    n = len(a)
    max_diag = max(a[i][i] for i in range(n))
    if not max_diag > 0.0:
        return None
    l = zeros(n, n)
    for j in range(n):
        d = a[j][j] - sum(l[j][k] ** 2 for k in range(j))
        if not d > CHOL_REL_EPS * max_diag:
            return None
        l[j][j] = math.sqrt(d)
        for i in range(j + 1, n):
            l[i][j] = (a[i][j] - sum(l[i][k] * l[j][k] for k in range(j))) / l[j][j]
    return l


def chol_solve(l, b):
    n = len(l)
    y = [0.0] * n
    for i in range(n):
        y[i] = (b[i] - sum(l[i][k] * y[k] for k in range(i))) / l[i][i]
    x = [0.0] * n
    for i in range(n - 1, -1, -1):
        x[i] = (y[i] - sum(l[k][i] * x[k] for k in range(i + 1, n))) / l[i][i]
    return x


def inv_spd(a):
    l = cholesky(a)
    if l is None:
        return None
    n = len(a)
    cols = [chol_solve(l, [1.0 if i == j else 0.0 for i in range(n)]) for j in range(n)]
    inv = [[cols[j][i] for j in range(n)] for i in range(n)]
    symmetrize(inv)
    return inv


# ---------------------------------------------------------- multilateração

def range_diff(cfg, p, a):
    z = p[2] if cfg.dim == 3 else cfg.tag_height_m
    diff = [p[0] - a[0], p[1] - a[1], z - a[2]]
    return diff, math.sqrt(sum(d * d for d in diff))


def solve_linear(cfg, ranges):
    dim = cfg.dim
    a0 = ranges[0].anchor
    d0sq = ranges[0].distance_m ** 2
    if dim == 2:
        d0sq = max(d0sq - (cfg.tag_height_m - a0[2]) ** 2, 0.0)
    ata = zeros(dim, dim)
    atb = [0.0] * dim
    for r in ranges[1:]:
        ai = r.anchor
        disq = r.distance_m ** 2
        if dim == 2:
            disq = max(disq - (cfg.tag_height_m - ai[2]) ** 2, 0.0)
        g = [ai[j] - a0[j] for j in range(dim)]
        b = d0sq - disq + sum(v * v for v in g)
        for j in range(dim):
            for k in range(dim):
                ata[j][k] += 4.0 * g[j] * g[k]
            atb[j] += 2.0 * g[j] * b
    l = cholesky(ata)
    if l is None:
        return None
    q = chol_solve(l, atb)
    pos = [a0[j] + q[j] for j in range(dim)]
    if dim == 2:
        pos.append(cfg.tag_height_m)
    return pos


def solve(cfg, ranges):
    """LS + Gauss-Newton: (posição [3], (JᵀJ)⁻¹, s²) ou None, como solve() em loc.c."""
    dim = cfg.dim
    n = min(len(ranges), 8)
    if n < dim + 1:
        return None
    pos = solve_linear(cfg, ranges[:n])
    if pos is None:
        return None

    converged = False
    it = 0
    while True:
        jtj = zeros(dim, dim)
        jtr = [0.0] * dim
        rss = 0.0
        used = 0
        for r in ranges[:n]:
            diff, pred = range_diff(cfg, pos, r.anchor)
            if pred < MIN_RANGE_M:
                continue
            res = r.distance_m - pred
            u = [diff[j] / pred for j in range(dim)]
            for j in range(dim):
                for k in range(dim):
                    jtj[j][k] += u[j] * u[k]
                jtr[j] += u[j] * res
            rss += res * res
            used += 1
        l = cholesky(jtj) if used >= dim + 1 else None
        if l is None:
            return None
        if it == GN_ITERS or converged:
            s2 = rss / (used - dim)
            break
        delta = chol_solve(l, jtr)
        for j in range(dim):
            pos[j] += delta[j]
        converged = sum(d * d for d in delta) < GN_STOP_M ** 2
        it += 1

    cov = inv_spd(jtj)
    return (pos, cov, s2) if cov is not None else None


# ---------------------------------------------------------------------- EKF

def nlos_var_factor(cfg, r):
    if not cfg.nlos_weighting or (r.rx_power_cdbm == 0 and r.resp_fp_power_cdbm == 0):
        return 1.0
    dp = (r.rx_power_cdbm - r.resp_fp_power_cdbm) / 100.0
    if dp <= cfg.nlos_los_db:
        return 1.0
    if dp >= cfg.nlos_nlos_db:
        return cfg.nlos_var_gain
    return 1.0 + (cfg.nlos_var_gain - 1.0) * (dp - cfg.nlos_los_db) / (cfg.nlos_nlos_db -
                                                                          cfg.nlos_los_db)


class Ekf:
    """rtls_loc_ctx + rtls_loc_update()."""

    def __init__(self, cfg):
        self.cfg = cfg
        self.n = 2 * cfg.dim
        self.stats = dict(updates=0, accepted=0, rejected=0, resets=0, nlos_weighted=0)
        self.reset()

    def reset(self):
        self.initialized = False
        self.last_t_ms = 0
        self.last_accept_ms = 0
        self.reject_streak = 0
        self.x = [0.0] * self.n
        self.P = zeros(self.n, self.n)

    def _restart(self):
        self.reset()
        self.stats["resets"] += 1

    def _init_from(self, t_ms, ranges):
        sol = solve(self.cfg, ranges)
        if sol is None:
            return False
        pos, cov, s2 = sol
        dim = self.cfg.dim
        var0 = max(self.cfg.range_std_m ** 2, s2)
        self.x = [0.0] * self.n
        self.P = zeros(self.n, self.n)
        for i in range(dim):
            self.x[i] = pos[i]
            for j in range(dim):
                self.P[i][j] = var0 * cov[i][j]
            self.P[dim + i][dim + i] = INIT_VEL_STD_MPS ** 2
        self.initialized = True
        self.last_t_ms = t_ms
        self.last_accept_ms = t_ms
        self.reject_streak = 0
        return True

    def _predict(self, dt):
        if not dt > 0.0:
            return
        dim, n = self.cfg.dim, self.n
        qa = self.cfg.accel_std_mps2 ** 2
        for i in range(dim):
            self.x[i] += dt * self.x[dim + i]
        f = identity(n)
        for i in range(dim):
            f[i][dim + i] = dt
        self.P = mul_bt(mul(f, self.P), f)
        dt2 = dt * dt
        for i in range(dim):
            self.P[i][i] += qa * dt2 * dt2 / 4.0
            self.P[i][dim + i] += qa * dt2 * dt / 2.0
            self.P[dim + i][i] += qa * dt2 * dt / 2.0
            self.P[dim + i][dim + i] += qa * dt2

    def _update_range(self, r):
        dim, n = self.cfg.dim, self.n
        vf = nlos_var_factor(self.cfg, r)
        r2 = self.cfg.range_std_m ** 2 * vf
        if vf > 1.0:
            self.stats["nlos_weighted"] += 1
        diff, pred = range_diff(self.cfg, self.x, r.anchor)
        if pred < MIN_RANGE_M:
            return False
        h = [diff[j] / pred for j in range(dim)]
        ph = [sum(self.P[i][j] * h[j] for j in range(dim)) for i in range(n)]
        s = r2 + sum(h[j] * ph[j] for j in range(dim))
        nu = r.distance_m - pred
        if not nu * nu <= self.cfg.gate_chi2 * s:
            return False
        k = [ph[i] / s for i in range(n)]
        for i in range(n):
            self.x[i] += k[i] * nu
        ikh = identity(n)
        for i in range(n):
            for j in range(dim):
                ikh[i][j] -= k[i] * h[j]
        self.P = mul_bt(mul(ikh, self.P), ikh)
        for i in range(n):
            for j in range(n):
                self.P[i][j] += r2 * k[i] * k[j]
        return True

    def _state_ok(self):
        pos_var = sum(self.P[i][i] for i in range(self.cfg.dim))
        return pos_var <= MAX_POS_VAR_M2 and all(math.isfinite(v) for v in self.x)

    def _consistent(self, ranges):
        sol = solve(self.cfg, ranges)
        return sol is not None and sol[2] <= self.cfg.gate_chi2 * self.cfg.range_std_m ** 2

    def update(self, t_ms, ranges):
        """Um ciclo. Devolve (pos[3], std[3], n_aceitas, flags) ou None (-EAGAIN)."""
        ranges = ranges[:8]
        n = len(ranges)
        accepted = rejected = 0
        flags = 0

        if self.initialized and t_ms - self.last_accept_ms > TIMEOUT_MS:
            self._restart()

        if self.initialized:
            self._predict((t_ms - self.last_t_ms) / 1000.0)
            self.last_t_ms = t_ms
            # da mais forte para a mais fraca (estável, como a ordenação em C)
            for r in sorted(ranges, key=lambda rr: -rr.rx_power_cdbm):
                if self._update_range(r):
                    accepted += 1
                else:
                    rejected += 1
            symmetrize(self.P)
            self.stats["accepted"] += accepted
            self.stats["rejected"] += rejected
            if rejected:
                flags |= FLAG_OUTLIER
            if accepted:
                self.last_accept_ms = t_ms
            if n > 0:
                self.reject_streak = self.reject_streak + 1 if 2 * rejected >= n else 0
            if not self._state_ok():
                self._restart()
            elif self.reject_streak > MAX_REJECT_STREAK and self._consistent(ranges):
                self._restart()

        if not self.initialized:
            if not self._init_from(t_ms, ranges):
                return None
            accepted = n
            self.stats["accepted"] += accepted
            flags = FLAG_INIT

        dim = self.cfg.dim
        pos = [self.x[i] for i in range(dim)]
        std = [math.sqrt(max(self.P[i][i], 0.0)) for i in range(dim)]
        if dim == 2:
            pos.append(self.cfg.tag_height_m)
            std.append(0.0)
        flags |= FLAG_VALID | (FLAG_3D if dim == 3 else 0)
        self.stats["updates"] += 1
        return pos, std, accepted, flags

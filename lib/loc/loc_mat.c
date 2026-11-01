#include <errno.h>
#include <math.h>
#include <string.h>

#include <rtls/loc_mat.h>

/* Pivô mínimo do Cholesky, relativo à maior diagonal (float: eps ~1,2e-7). */
#define CHOL_REL_EPS 1e-6f

void rtls_mat_zero(struct rtls_mat *m, uint8_t rows, uint8_t cols)
{
	memset(m, 0, sizeof(*m));
	m->rows = rows;
	m->cols = cols;
}

void rtls_mat_identity(struct rtls_mat *m, uint8_t n)
{
	rtls_mat_zero(m, n, n);
	for (int i = 0; i < n; i++) {
		m->a[i][i] = 1.0f;
	}
}

int rtls_mat_mul(struct rtls_mat *c, const struct rtls_mat *a, const struct rtls_mat *b)
{
	if (a->cols != b->rows) {
		return -EINVAL;
	}

	c->rows = a->rows;
	c->cols = b->cols;
	for (int i = 0; i < a->rows; i++) {
		for (int j = 0; j < b->cols; j++) {
			float s = 0.0f;

			for (int k = 0; k < a->cols; k++) {
				s += a->a[i][k] * b->a[k][j];
			}
			c->a[i][j] = s;
		}
	}

	return 0;
}

int rtls_mat_mul_bt(struct rtls_mat *c, const struct rtls_mat *a, const struct rtls_mat *b)
{
	if (a->cols != b->cols) {
		return -EINVAL;
	}

	c->rows = a->rows;
	c->cols = b->rows;
	for (int i = 0; i < a->rows; i++) {
		for (int j = 0; j < b->rows; j++) {
			float s = 0.0f;

			for (int k = 0; k < a->cols; k++) {
				s += a->a[i][k] * b->a[j][k];
			}
			c->a[i][j] = s;
		}
	}

	return 0;
}

void rtls_mat_symmetrize(struct rtls_mat *m)
{
	for (int i = 0; i < m->rows; i++) {
		for (int j = i + 1; j < m->cols; j++) {
			const float s = 0.5f * (m->a[i][j] + m->a[j][i]);

			m->a[i][j] = s;
			m->a[j][i] = s;
		}
	}
}

float rtls_mat_trace(const struct rtls_mat *m)
{
	float s = 0.0f;

	for (int i = 0; i < m->rows && i < m->cols; i++) {
		s += m->a[i][i];
	}

	return s;
}

int rtls_mat_cholesky(struct rtls_mat *l, const struct rtls_mat *a)
{
	const int n = a->rows;
	float max_diag = 0.0f;

	if (a->rows != a->cols) {
		return -EINVAL;
	}

	for (int i = 0; i < n; i++) {
		max_diag = fmaxf(max_diag, a->a[i][i]);
	}
	/* "!(x > 0)" também pega NaN */
	if (!(max_diag > 0.0f)) {
		return -EDOM;
	}

	rtls_mat_zero(l, a->rows, a->cols);
	for (int j = 0; j < n; j++) {
		float d = a->a[j][j];

		for (int k = 0; k < j; k++) {
			d -= l->a[j][k] * l->a[j][k];
		}
		if (!(d > CHOL_REL_EPS * max_diag)) {
			return -EDOM;
		}
		l->a[j][j] = sqrtf(d);

		for (int i = j + 1; i < n; i++) {
			float s = a->a[i][j];

			for (int k = 0; k < j; k++) {
				s -= l->a[i][k] * l->a[j][k];
			}
			l->a[i][j] = s / l->a[j][j];
		}
	}

	return 0;
}

void rtls_mat_chol_solve(const struct rtls_mat *l, const float *b, float *x)
{
	const int n = l->rows;
	float y[RTLS_MAT_MAX];

	/* l y = b (substituição direta) */
	for (int i = 0; i < n; i++) {
		float s = b[i];

		for (int k = 0; k < i; k++) {
			s -= l->a[i][k] * y[k];
		}
		y[i] = s / l->a[i][i];
	}

	/* l^T x = y (substituição reversa) */
	for (int i = n - 1; i >= 0; i--) {
		float s = y[i];

		for (int k = i + 1; k < n; k++) {
			s -= l->a[k][i] * x[k];
		}
		x[i] = s / l->a[i][i];
	}
}

int rtls_mat_inv_spd(struct rtls_mat *inv, const struct rtls_mat *a)
{
	struct rtls_mat l;
	float col[RTLS_MAT_MAX];
	int err;

	err = rtls_mat_cholesky(&l, a);
	if (err) {
		return err;
	}

	rtls_mat_zero(inv, a->rows, a->cols);
	for (int j = 0; j < a->rows; j++) {
		memset(col, 0, sizeof(col));
		col[j] = 1.0f;
		rtls_mat_chol_solve(&l, col, col);
		for (int i = 0; i < a->rows; i++) {
			inv->a[i][j] = col[i];
		}
	}
	rtls_mat_symmetrize(inv);

	return 0;
}

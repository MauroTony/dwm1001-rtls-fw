/*
 * Matrizes pequenas em float (até 6x6) para a localização: produto,
 * transposta no segundo fator, Cholesky e inversa de matriz simétrica
 * positiva definida. Sem alocação: o chamador fornece todas as matrizes.
 * Uso: lib/loc/loc.c (docs/04-algoritmos.md, seções 4 e 5).
 */

#ifndef RTLS_LOC_MAT_H_
#define RTLS_LOC_MAT_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTLS_MAT_MAX 6

struct rtls_mat {
	uint8_t rows;
	uint8_t cols;
	float a[RTLS_MAT_MAX][RTLS_MAT_MAX];
};

/** Zera @p m e define as dimensões. */
void rtls_mat_zero(struct rtls_mat *m, uint8_t rows, uint8_t cols);

/** Matriz identidade n x n. */
void rtls_mat_identity(struct rtls_mat *m, uint8_t n);

/** c = a b. @p c não pode ser @p a nem @p b. */
int rtls_mat_mul(struct rtls_mat *c, const struct rtls_mat *a, const struct rtls_mat *b);

/** c = a b^T. @p c não pode ser @p a nem @p b. */
int rtls_mat_mul_bt(struct rtls_mat *c, const struct rtls_mat *a, const struct rtls_mat *b);

/** m = (m + m^T) / 2 (só matriz quadrada). */
void rtls_mat_symmetrize(struct rtls_mat *m);

/** Soma da diagonal. */
float rtls_mat_trace(const struct rtls_mat *m);

/**
 * Fator de Cholesky: a = l l^T, com l triangular inferior.
 *
 * @retval -EDOM @p a não é positiva definida (pivô <= 1e-6 x maior diagonal):
 *         matriz singular ou geometria degenerada
 */
int rtls_mat_cholesky(struct rtls_mat *l, const struct rtls_mat *a);

/** Resolve l l^T x = b, com @p l vindo de rtls_mat_cholesky(). x pode ser b. */
void rtls_mat_chol_solve(const struct rtls_mat *l, const float *b, float *x);

/**
 * Inversa de matriz simétrica positiva definida, por Cholesky.
 *
 * @retval -EDOM @p a não é positiva definida
 */
int rtls_mat_inv_spd(struct rtls_mat *inv, const struct rtls_mat *a);

#ifdef __cplusplus
}
#endif

#endif /* RTLS_LOC_MAT_H_ */

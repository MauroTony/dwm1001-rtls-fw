/*
 * Testes do registro de parâmetros (M8): T8.0 em docs/08, seção 11. Nos
 * testes só existem os parâmetros das bibliotecas (PAN e EKF); os de cada
 * papel (ID, âncora, antena) entram nos firmwares.
 */

#include <errno.h>

#include <zephyr/ztest.h>

#include <rtls/config.h>

/* T8.0 */
ZTEST(config, test_find_by_name_and_key)
{
	const struct rtls_cfg_param *by_name = rtls_config_find("loc/range_std_mm");
	const struct rtls_cfg_param *by_key = rtls_config_find("0x0603");

	zassert_not_null(by_name);
	zassert_equal_ptr(by_name, by_key);
	zassert_equal(by_name->key, RTLS_CFG_LOC_RANGE_STD);
	zassert_equal(by_name->def, CONFIG_RTLS_LOC_RANGE_STD_MM);

	zassert_is_null(rtls_config_find("loc/nao_existe"));
	zassert_is_null(rtls_config_find("0x9999"));
	zassert_is_null(rtls_config_find("0x06zz"));
	zassert_is_null(rtls_config_find(NULL));

	/* parâmetro de outro papel não aparece neste firmware */
	zassert_is_null(rtls_config_find("anchor/pos_x_mm"));
	zassert_equal(rtls_config_index(RTLS_CFG_ANCHOR_POS_X), -ENOENT);
}

/* T8.0 */
ZTEST(config, test_validate_ranges)
{
	const struct rtls_cfg_param *pan = rtls_config_find("node/pan");
	const struct rtls_cfg_param *std = rtls_config_find("loc/range_std_mm");
	const struct rtls_cfg_param *mode = rtls_config_find("loc/mode_3d");

	zassert_not_null(pan);
	zassert_ok(rtls_config_validate(pan, 0xDECA));
	zassert_equal(rtls_config_validate(pan, 0xFFFF), -ERANGE, "PAN de broadcast");
	zassert_equal(pan->cls, RTLS_CFG_COORD);
	zassert_true(pan->hex);

	zassert_ok(rtls_config_validate(std, 50));
	zassert_equal(rtls_config_validate(std, 0), -ERANGE);
	zassert_equal(rtls_config_validate(std, 5001), -ERANGE);

	zassert_ok(rtls_config_validate(mode, 1));
	zassert_equal(rtls_config_validate(mode, 2), -ERANGE);
}

/* T8.0: chaves únicas e padrões dentro da própria faixa */
ZTEST(config, test_table_is_consistent)
{
	const size_t n = rtls_config_count();

	zassert_true(n > 0);
	zassert_is_null(rtls_config_param(n));

	for (size_t i = 0; i < n; i++) {
		const struct rtls_cfg_param *p = rtls_config_param(i);

		zassert_true(p->def >= p->min && p->def <= p->max, "%s: padrao fora da faixa",
			     p->name);
		zassert_equal(rtls_config_index(p->key), (int)i, "%s: chave repetida", p->name);
		zassert_equal_ptr(rtls_config_find(p->name), p);
	}
}

ZTEST_SUITE(config, NULL, NULL, NULL, NULL, NULL);

/*
 * Valores do registro e persistência na NVS pelo subsistema settings
 * (subárvore "rtls/<nome>", um i32 por parâmetro).
 */

#include <errno.h>
#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>

#include <rtls/config.h>

LOG_MODULE_REGISTER(rtls_config, CONFIG_RTLS_CONFIG_LOG_LEVEL);

/* Um bit por parâmetro nas máscaras de "salvo" e "mudou". */
#define MAX_PARAMS 32

static int32_t values[MAX_PARAMS];
static atomic_t saved_mask;
static atomic_t changed_mask;
static K_MUTEX_DEFINE(save_lock);

static int settings_key(const struct rtls_cfg_param *p, char *buf, size_t size)
{
	const int n = snprintf(buf, size, "rtls/%s", p->name);

	return (n > 0 && (size_t)n < size) ? 0 : -ENAMETOOLONG;
}

/* Chamado por settings_load_subtree() para cada "rtls/<nome>" salvo. */
static int h_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	const struct rtls_cfg_param *p = rtls_config_find(name);
	int32_t v;
	int idx;

	if (p == NULL) {
		LOG_WRN("NVS: parametro desconhecido rtls/%s (ignorado)", name);
		return 0;
	}
	if (len != sizeof(v) || read_cb(cb_arg, &v, sizeof(v)) != sizeof(v)) {
		return -EINVAL;
	}
	/* Faixa conferida de novo: um firmware novo pode ter apertado o limite. */
	if (p->cls == RTLS_CFG_READONLY || v < p->min || v > p->max) {
		LOG_WRN("NVS: %s = %d fora da faixa (ignorado)", p->name, v);
		return 0;
	}

	idx = rtls_config_index(p->key);
	values[idx] = v;
	atomic_set_bit(&saved_mask, idx);
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(rtls, "rtls", NULL, h_set, NULL, NULL);

int rtls_config_init(void)
{
	const size_t n = rtls_config_count();
	int err;

	BUILD_ASSERT(MAX_PARAMS <= ATOMIC_BITS, "mascara de 32 bits");
	if (n > MAX_PARAMS) {
		return -ENOMEM;
	}

	for (size_t i = 0; i < n; i++) {
		values[i] = rtls_config_param(i)->def;
	}
	atomic_clear(&saved_mask);
	atomic_clear(&changed_mask);

	err = settings_subsys_init();
	if (err == 0) {
		err = settings_load_subtree("rtls");
	}
	if (err) {
		LOG_ERR("NVS indisponivel (%d): usando os padroes do Kconfig", err);
		return err;
	}

	LOG_INF("config: %zu parametros, %u salvos na NVS", n,
		(unsigned int)__builtin_popcount((unsigned int)atomic_get(&saved_mask)));
	return 0;
}

int32_t rtls_config_get(uint16_t key)
{
	const int idx = rtls_config_index(key);

	return (idx >= 0) ? values[idx] : 0;
}

bool rtls_config_is_saved(uint16_t key)
{
	const int idx = rtls_config_index(key);

	return idx >= 0 && atomic_test_bit(&saved_mask, idx);
}

int rtls_config_set(uint16_t key, int32_t value)
{
	const int idx = rtls_config_index(key);
	const struct rtls_cfg_param *p;
	char skey[48];
	int err;

	if (idx < 0) {
		return -ENOENT;
	}
	p = rtls_config_param(idx);
	err = rtls_config_validate(p, value);
	if (err == 0) {
		err = settings_key(p, skey, sizeof(skey));
	}
	if (err) {
		return err;
	}

	k_mutex_lock(&save_lock, K_FOREVER);
	err = settings_save_one(skey, &value, sizeof(value));
	if (err == 0) {
		values[idx] = value;
		atomic_set_bit(&saved_mask, idx);
		atomic_set_bit(&changed_mask, idx);
	}
	k_mutex_unlock(&save_lock);

	return err;
}

static int reset_one(int idx)
{
	const struct rtls_cfg_param *p = rtls_config_param(idx);
	char skey[48];
	int err;

	err = settings_key(p, skey, sizeof(skey));
	if (err == 0 && atomic_test_bit(&saved_mask, idx)) {
		err = settings_delete(skey);
	}
	if (err == 0) {
		atomic_clear_bit(&saved_mask, idx);
		if (values[idx] != p->def) {
			values[idx] = p->def;
			atomic_set_bit(&changed_mask, idx);
		}
	}

	return err;
}

int rtls_config_reset(uint16_t key)
{
	int err = 0;

	k_mutex_lock(&save_lock, K_FOREVER);
	if (key == 0) {
		for (size_t i = 0; i < rtls_config_count() && err == 0; i++) {
			err = reset_one((int)i);
		}
	} else {
		const int idx = rtls_config_index(key);

		err = (idx >= 0) ? reset_one(idx) : -ENOENT;
	}
	k_mutex_unlock(&save_lock);

	return err;
}

uint16_t rtls_config_next_change(void)
{
	for (size_t i = 0; i < rtls_config_count(); i++) {
		if (atomic_test_and_clear_bit(&changed_mask, i)) {
			return rtls_config_param(i)->key;
		}
	}

	return 0;
}

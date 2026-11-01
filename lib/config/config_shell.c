/*
 * Comandos "rtls" do shell (M8): ver e alterar o registro de parâmetros,
 * reiniciar e testar o watchdog. Textos sem acentos (terminais do Windows).
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>

#include <rtls/config.h>
#include <rtls/watchdog.h>

static const char *const class_names[] = {
	[RTLS_CFG_IMMEDIATE] = "imediato",
	[RTLS_CFG_REBOOT] = "reinicio",
	[RTLS_CFG_COORD] = "coordenado",
	[RTLS_CFG_READONLY] = "leitura",
};

static void print_param(const struct shell *sh, const struct rtls_cfg_param *p)
{
	const int32_t v = rtls_config_get(p->key);
	const char *origin = rtls_config_is_saved(p->key) ? "NVS" : "padrao";

	if (p->hex) {
		shell_print(sh, "0x%04x  %-20s  0x%04x  (%s, %s)", p->key, p->name, v, origin,
			    class_names[p->cls]);
	} else {
		shell_print(sh, "0x%04x  %-20s  %6d  (%s, %s)", p->key, p->name, v, origin,
			    class_names[p->cls]);
	}
}

static const struct rtls_cfg_param *find_or_complain(const struct shell *sh, const char *arg)
{
	const struct rtls_cfg_param *p = rtls_config_find(arg);

	if (p == NULL) {
		shell_error(sh, "parametro desconhecido: %s (veja 'rtls show')", arg);
	}
	return p;
}

static int cmd_show(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	for (size_t i = 0; i < rtls_config_count(); i++) {
		print_param(sh, rtls_config_param(i));
	}
	return 0;
}

static int cmd_get(const struct shell *sh, size_t argc, char **argv)
{
	const struct rtls_cfg_param *p = find_or_complain(sh, argv[1]);

	ARG_UNUSED(argc);
	if (p == NULL) {
		return -ENOENT;
	}
	print_param(sh, p);
	return 0;
}

static int cmd_set(const struct shell *sh, size_t argc, char **argv)
{
	const struct rtls_cfg_param *p = find_or_complain(sh, argv[1]);
	char *end;
	long value;
	int err;

	ARG_UNUSED(argc);
	if (p == NULL) {
		return -ENOENT;
	}

	/* Aceita decimal e 0x...; IDs costumam vir em hexadecimal. */
	value = strtol(argv[2], &end, 0);
	if (*end != '\0') {
		shell_error(sh, "valor invalido: %s", argv[2]);
		return -EINVAL;
	}

	err = rtls_config_set(p->key, (int32_t)value);
	if (err == -EACCES) {
		shell_error(sh, "%s e somente leitura", p->name);
		return err;
	}
	if (err == -ERANGE) {
		shell_error(sh, "%s fora da faixa [%d, %d]", p->name, p->min, p->max);
		return err;
	}
	if (err) {
		shell_error(sh, "falha ao salvar na NVS (%d)", err);
		return err;
	}

	switch (p->cls) {
	case RTLS_CFG_IMMEDIATE:
		shell_print(sh, "%s = %s: salvo e aplicado", p->name, argv[2]);
		break;
	case RTLS_CFG_COORD:
		shell_warn(sh, "%s = %s: salvo; precisa ser igual em TODOS os nos e vale depois de "
			   "'rtls reboot'",
			   p->name, argv[2]);
		break;
	default:
		shell_print(sh, "%s = %s: salvo; vale depois de 'rtls reboot'", p->name, argv[2]);
		break;
	}
	return 0;
}

static int cmd_reset(const struct shell *sh, size_t argc, char **argv)
{
	const struct rtls_cfg_param *p = NULL;
	int err;

	ARG_UNUSED(argc);
	if (strcmp(argv[1], "all") != 0) {
		p = find_or_complain(sh, argv[1]);
		if (p == NULL) {
			return -ENOENT;
		}
	}

	err = rtls_config_reset((p != NULL) ? p->key : 0);
	if (err) {
		shell_error(sh, "falha ao apagar da NVS (%d)", err);
		return err;
	}
	shell_print(sh, "%s de volta ao padrao do Kconfig (reinicie para os de classe 'reinicio')",
		    (p != NULL) ? p->name : "todos os parametros");
	return 0;
}

static int cmd_reboot(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "reiniciando...");
	k_sleep(K_MSEC(100)); /* deixa a mensagem sair pela UART */
	sys_reboot(SYS_REBOOT_COLD);
	return 0;
}

#if defined(CONFIG_RTLS_WATCHDOG)
static int cmd_hang(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_warn(sh, "travando a thread main: o watchdog deve reiniciar o no em %d ms (T8.2)",
		   CONFIG_RTLS_WATCHDOG_TIMEOUT_MS);
	k_sleep(K_MSEC(100));
	rtls_watchdog_hang();
	return 0;
}
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_rtls, SHELL_CMD_ARG(show, NULL, "Lista os parametros e a origem do valor", cmd_show, 1, 0),
	SHELL_CMD_ARG(get, NULL, "Mostra um parametro: rtls get <nome|0xchave>", cmd_get, 2, 0),
	SHELL_CMD_ARG(set, NULL, "Grava na NVS: rtls set <nome|0xchave> <valor>", cmd_set, 3, 0),
	SHELL_CMD_ARG(reset, NULL, "Volta ao padrao: rtls reset <nome|0xchave|all>", cmd_reset, 2,
		      0),
	SHELL_CMD_ARG(reboot, NULL, "Reinicia o no", cmd_reboot, 1, 0),
#if defined(CONFIG_RTLS_WATCHDOG)
	SHELL_CMD_ARG(hang, NULL, "Trava a thread main para testar o watchdog", cmd_hang, 1, 0),
#endif
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(rtls, &sub_rtls, "Parametros do no RTLS (M8)", NULL);

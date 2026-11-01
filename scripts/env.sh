# Ativa o ambiente do firmware RTLS no shell atual (bash).
#
# Uso (na raiz do repositório):
#     source scripts/env.sh

_rtls_repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
_rtls_ws="$(dirname "$_rtls_repo")"

if [ ! -f "$_rtls_ws/.venv/bin/activate" ]; then
	echo "venv não encontrado em $_rtls_ws/.venv. Rode antes: scripts/setup/setup-linux.sh" >&2
	unset _rtls_repo _rtls_ws
	return 1 2>/dev/null || exit 1
fi

# shellcheck disable=SC1091
. "$_rtls_ws/.venv/bin/activate"

if [ -d /opt/SEGGER/JLink ]; then
	case ":$PATH:" in
	*":/opt/SEGGER/JLink:"*) ;;
	*) export PATH="/opt/SEGGER/JLink:$PATH" ;;
	esac
fi

# Zephyr SDK explícito: sem depender do registro de pacotes do CMake.
if [ -z "${ZEPHYR_SDK_INSTALL_DIR:-}" ] && [ -f "$_rtls_ws/zephyr/SDK_VERSION" ]; then
	_rtls_sdk="$HOME/zephyr-sdk-$(head -n1 "$_rtls_ws/zephyr/SDK_VERSION" | tr -d '[:space:]')"
	if [ -f "$_rtls_sdk/cmake/Zephyr-sdkConfig.cmake" ]; then
		export ZEPHYR_SDK_INSTALL_DIR="$_rtls_sdk"
	else
		echo "Zephyr SDK não encontrado em $_rtls_sdk. Rode antes: scripts/setup/setup-linux.sh" >&2
	fi
	unset _rtls_sdk
fi

cd "$_rtls_repo" || return 1
echo "Ambiente RTLS ativo (workspace west: $_rtls_ws)"
echo "  west build -b decawave_dwm1001_dev apps/tag -d build/tag"
echo "  west flash -d build/tag [--dev-id <serial J-Link>]"
echo "  scripts/list-boards.sh   (placas conectadas: porta x serial)"
unset _rtls_repo _rtls_ws

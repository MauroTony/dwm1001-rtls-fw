#!/usr/bin/env bash
#
# Configura o ambiente de desenvolvimento do firmware RTLS (Zephyr) no Linux.
# Testado como alvo: Ubuntu/Debian com apt (Ubuntu 24.04+ recomendado).
#
# Passos (cada um é pulado se já estiver feito):
#   1. Pacotes do sistema (apt).
#   2. Python >= 3.12.
#   3. Workspace west (topologia T2): o workspace é o diretório PAI deste
#      repositório.
#   4. venv em <workspace>/.venv + west (+ CMake via pip se o do sistema for antigo).
#   5. Zephyr e módulos fixados no west.yml, dependências Python, export CMake.
#   6. Zephyr SDK, apenas a toolchain arm-zephyr-eabi (+ host tools).
#   7. Acesso à porta serial (grupo dialout) e verificação do SEGGER J-Link.
#   8. Build de teste dos três firmwares.
#
# Documentação completa: docs/06-configuracao-ambiente.md

set -euo pipefail

PYTHON_MIN="3.12"
CMAKE_MIN="3.20.5"
DTC_MIN="1.4.6"
BOARD="decawave_dwm1001_dev"
APPS=(tag anchor gateway)

YES=0
SKIP_PACKAGES=0
SKIP_SDK=0
SKIP_BUILD=0
FULL_CLONE=0

usage() {
	cat <<EOF
Uso: $(basename "$0") [opções]

  -y, --yes          responde "sim" às confirmações
  --skip-packages    não instala pacotes do sistema (apt)
  --skip-sdk         não instala o Zephyr SDK
  --skip-build       não executa o build de teste
  --full-clone       clona o histórico completo do Zephyr (padrão: raso)
  -h, --help         mostra esta ajuda
EOF
}

while [[ $# -gt 0 ]]; do
	case "$1" in
	-y | --yes) YES=1 ;;
	--skip-packages) SKIP_PACKAGES=1 ;;
	--skip-sdk) SKIP_SDK=1 ;;
	--skip-build) SKIP_BUILD=1 ;;
	--full-clone) FULL_CLONE=1 ;;
	-h | --help)
		usage
		exit 0
		;;
	*)
		echo "Opção desconhecida: $1" >&2
		usage
		exit 1
		;;
	esac
	shift
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
REPO_NAME="$(basename "$REPO_ROOT")"
WORKSPACE="$(dirname "$REPO_ROOT")"
VENV_DIR="$WORKSPACE/.venv"

step() { printf '\n\033[36m==> %s\033[0m\n' "$*"; }
info() { printf '    %s\n' "$*"; }
note() { printf '    \033[33mAVISO: %s\033[0m\n' "$*"; }
die() {
	printf '\033[31mERRO: %s\033[0m\n' "$*" >&2
	exit 1
}

confirm() {
	[[ $YES -eq 1 ]] && return 0
	local answer
	read -r -p "$1 [s/N] " answer
	[[ "$answer" =~ ^([sSyY]|sim|yes)$ ]]
}

# version_ge A B: verdadeiro se A >= B.
version_ge() {
	[[ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -n1)" == "$2" ]]
}

tool_version() {
	local out
	out="$("$1" --version 2>/dev/null)" || return 1
	[[ "$out" =~ ([0-9]+\.[0-9]+(\.[0-9]+)?) ]] && echo "${BASH_REMATCH[1]}"
}

find_python() {
	local candidate version
	for candidate in python3 python3.13 python3.12; do
		if command -v "$candidate" >/dev/null 2>&1; then
			version="$("$candidate" -c 'import sys; print("%d.%d" % sys.version_info[:2])')"
			if version_ge "$version" "$PYTHON_MIN"; then
				command -v "$candidate"
				return 0
			fi
		fi
	done
	return 1
}

echo "Firmware RTLS DWM1001 - setup do ambiente (Linux)"
info "Repositório: $REPO_ROOT"
info "Workspace west: $WORKSPACE"

# ---------------------------------------------------------------------------
step "1/8 Pacotes do sistema"
if [[ $SKIP_PACKAGES -eq 1 ]]; then
	info "Pulado (--skip-packages)"
elif command -v apt-get >/dev/null 2>&1; then
	packages=(git cmake ninja-build gperf ccache dfu-util device-tree-compiler wget
		python3-dev python3-venv python3-tk xz-utils file make gcc libsdl2-dev libmagic1)
	if [[ "$(uname -m)" == "x86_64" ]]; then
		packages+=(gcc-multilib g++-multilib)
	fi
	sudo apt-get update
	sudo apt-get install -y --no-install-recommends "${packages[@]}"
else
	note "Distribuição sem apt-get. Instale manualmente os equivalentes de:"
	info "git cmake ninja gperf ccache dfu-util dtc wget python3 (dev + venv) xz file make gcc"
fi

for tool in git ninja gperf dtc; do
	command -v "$tool" >/dev/null 2>&1 || die "'$tool' não encontrado no PATH."
done
dtc_version="$(tool_version dtc)"
version_ge "$dtc_version" "$DTC_MIN" || die "dtc $dtc_version é antigo demais (mínimo $DTC_MIN)."
info "dtc $dtc_version"

# ---------------------------------------------------------------------------
step "2/8 Python >= $PYTHON_MIN"
PYTHON="$(find_python)" || die "Python >= $PYTHON_MIN não encontrado. No Ubuntu 22.04, use o PPA deadsnakes (python3.12, python3.12-venv) ou atualize para o 24.04."
info "Python: $PYTHON ($("$PYTHON" --version))"

# ---------------------------------------------------------------------------
step "3/8 Workspace west em $WORKSPACE"
if [[ -d "$WORKSPACE/.west" ]]; then
	info "Workspace west já existe"
else
	others=0
	for entry in "$WORKSPACE"/* "$WORKSPACE"/.[!.]*; do
		[[ -e "$entry" ]] || continue
		case "$(basename "$entry")" in
		"$REPO_NAME" | .venv | zephyr | modules) ;;
		*) others=$((others + 1)) ;;
		esac
	done
	if [[ $others -gt 0 ]]; then
		note "A pasta '$WORKSPACE' já contém $others outro(s) item(ns)."
		info "O west vai criar ali: .west/, .venv/, zephyr/ e modules/."
		info "Recomendado: deixar o repositório numa pasta dedicada, ex.: $WORKSPACE/rtls-ws/$REPO_NAME"
		confirm "Usar '$WORKSPACE' como workspace west mesmo assim?" ||
			die "Cancelado. Mova o repositório para uma pasta dedicada e rode o script de lá."
	fi
fi

# ---------------------------------------------------------------------------
step "4/8 Ambiente virtual Python ($VENV_DIR)"
if [[ ! -x "$VENV_DIR/bin/python" ]]; then
	"$PYTHON" -m venv "$VENV_DIR"
fi
# shellcheck disable=SC1091
source "$VENV_DIR/bin/activate"
python -m pip install --upgrade --quiet pip west

cmake_version="$(tool_version cmake || true)"
if [[ -z "$cmake_version" ]] || ! version_ge "$cmake_version" "$CMAKE_MIN"; then
	info "CMake do sistema ausente ou antigo (${cmake_version:-nenhum}); instalando via pip no venv"
	python -m pip install --quiet "cmake>=$CMAKE_MIN"
	cmake_version="$(tool_version cmake)"
fi
info "cmake $cmake_version"
info "$(west --version)"

# ---------------------------------------------------------------------------
step "5/8 Zephyr e módulos (west init / update)"
if [[ ! -d "$WORKSPACE/.west" ]]; then
	west init -l "$REPO_ROOT"
fi
cd "$REPO_ROOT"
update_args=(update)
if [[ $FULL_CLONE -eq 0 ]]; then
	update_args+=(--narrow --fetch-opt=--depth=1)
fi
west "${update_args[@]}"
west zephyr-export
info "Dependências Python do Zephyr..."
west packages pip --install

# ---------------------------------------------------------------------------
step "6/8 Zephyr SDK (toolchain arm-zephyr-eabi)"
if [[ $SKIP_SDK -eq 1 ]]; then
	info "Pulado (--skip-sdk)"
else
	sdk_version="$(head -n1 "$WORKSPACE/zephyr/SDK_VERSION" | tr -d '[:space:]')"
	sdk_dir="$HOME/zephyr-sdk-$sdk_version"
	if [[ -d "$sdk_dir/arm-zephyr-eabi" || -d "$sdk_dir/gnu/arm-zephyr-eabi" ]]; then
		info "SDK $sdk_version já instalado em $sdk_dir"
	else
		(cd "$WORKSPACE/zephyr" && west sdk install -t arm-zephyr-eabi)
	fi
fi

# ---------------------------------------------------------------------------
step "7/8 Porta serial e SEGGER J-Link"
current_user="${USER:-$(id -un)}"
if getent group dialout >/dev/null && ! id -nG "$current_user" | grep -qw dialout; then
	note "Usuário '$current_user' fora do grupo dialout (necessário para /dev/ttyACM*)."
	if confirm "Adicionar '$current_user' ao grupo dialout (requer sudo e novo login)?"; then
		sudo usermod -aG dialout "$current_user"
		info "Feito. Faça logout/login para valer."
	fi
else
	info "Acesso serial: ok"
fi

if command -v JLinkExe >/dev/null 2>&1 || [[ -x /opt/SEGGER/JLink/JLinkExe ]]; then
	info "J-Link: $(command -v JLinkExe || echo /opt/SEGGER/JLink/JLinkExe)"
else
	note "J-Link Software não encontrado."
	info "Baixe o .deb \"J-Link Software and Documentation Pack\" (Linux 64-bit DEB) em"
	info "https://www.segger.com/downloads/jlink/ e instale com:"
	info "    sudo apt install ./JLink_Linux_V*_x86_64.deb"
	info "O pacote também instala as regras udev do J-Link."
fi

# ---------------------------------------------------------------------------
step "8/8 Build de teste"
if [[ $SKIP_BUILD -eq 1 ]]; then
	info "Pulado (--skip-build)"
else
	cd "$REPO_ROOT"
	for app in "${APPS[@]}"; do
		info "apps/$app -> build/$app"
		west build -b "$BOARD" "apps/$app" -d "build/$app" -p auto
	done
fi

printf '\n\033[32mAmbiente pronto.\033[0m\n'
echo "Em cada terminal novo, ative com:"
echo "    source scripts/env.sh"
echo "Gravar a tag (com a placa conectada):"
echo "    west flash -d build/tag"
echo "Testes unitários (native_sim):"
echo "    west twister -T tests -p native_sim"

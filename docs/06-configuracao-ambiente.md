# 06 — Configuração do ambiente (Windows e Linux)

Este guia vai do zero até gravar e ver o log de uma DWM1001-DEV. Os scripts
de [`scripts/setup/`](../scripts/setup) automatizam tudo. As seções 4 e 5
documentam os mesmos passos manualmente, para entender o que é feito ou
resolver problemas.

| Componente | Versão | Onde é definida |
|------------|--------|-----------------|
| Zephyr RTOS | **v4.4.0** | [`west.yml`](../west.yml) |
| Zephyr SDK | **1.0.1** (toolchain `arm-zephyr-eabi`) | `zephyr/SDK_VERSION` (lido pelo `west sdk install`) |
| Python | 3.12 (mínimo exigido pelo Zephyr 4.4) | scripts de setup |
| CMake / dtc | ≥ 3.20.5 / ≥ 1.4.6 | scripts de setup |
| SEGGER J-Link | V7.x ou mais novo (testado com V9.86a) | instalação manual (licença SEGGER) |
| Placa (`-b`) | `decawave_dwm1001_dev` | Zephyr |

---

## 1. Organização do workspace

O repositório é o **manifest repository** do west (topologia T2). O
**workspace west é o diretório pai** do repositório:

```
<workspace>/                  ex.: C:\Workspace\rtls-ws  ou  ~/rtls-ws
├── .west/                    configuração do west
├── .venv/                    Python + west + dependências do Zephyr
├── dwm1001-rtls-fw/          ESTE repositório
│   └── build/<app>/          saídas de build (ignoradas pelo git)
├── zephyr/                   Zephyr v4.4.0
└── modules/hal/{cmsis_6,nordic}, modules/debug/segger
~/zephyr-sdk-1.0.1/           toolchain (fora do workspace)
```

> **Use uma pasta dedicada.** Se o repositório estiver numa pasta com outros
> projetos (ex.: `C:\Workspace\dwm1001-rtls-fw`), o west vai criar `.west`,
> `.venv`, `zephyr` e `modules` dentro de `C:\Workspace`. O script avisa e
> pede confirmação. O recomendado é:
>
> ```bash
> mkdir rtls-ws
> cd rtls-ws
> git clone https://github.com/MauroTony/dwm1001-rtls-fw.git
> ```

**Espaço em disco:** cerca de 1,5 GB (Zephyr em clone raso + módulos + SDK
só com ARM).

---

## 2. Windows (nativo)

### 2.1 Pré-requisitos

- Windows 10/11 com o **winget** (pacote "App Installer" da Microsoft Store).
- **SEGGER J-Link Software and Documentation Pack** (64-bit), baixado em
  <https://www.segger.com/downloads/jlink/> e instalado com as opções padrão.
  A instalação exige aceitar a licença da SEGGER, por isso é manual. Ela
  também instala o driver USB do J-Link OB e da porta COM virtual.

### 2.2 Setup automático

No PowerShell, a partir da raiz do repositório:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\setup\setup-windows.ps1
```

| Opção | Efeito |
|-------|--------|
| `-PythonVersion 3.13` | usa outra versão do Python no venv (padrão `3.12`) |
| `-SkipPackages` | não instala nada via winget |
| `-SkipSdk` | não instala o Zephyr SDK |
| `-SkipBuild` | pula o build de teste |
| `-FullClone` | histórico completo do Zephyr (padrão: clone raso, `--depth=1`) |
| `-AcceptAgreements` | aceita automaticamente os termos do winget |
| `-Yes` | não pergunta nada |

O que o script faz (cada passo é idempotente):

1. **winget:** `Kitware.CMake`, `Ninja-build.Ninja`, `oss-winget.gperf`,
   `oss-winget.dtc`, `Git.Git`, `7zip.7zip`. Só instala o que faltar no PATH.
   Se o winget falhar, o script segue: CMake e Ninja vêm do pip dentro do venv
   (passo 4). O dtc é opcional, e o gperf só é usado com `CONFIG_USERSPACE`.
   Só Git e 7-Zip são obrigatórios.
2. **Python 3.12:** usa o `py -3.12` se existir. Senão instala pelo *Python
   install manager* (`pymanager install 3.12`) ou pelo winget
   (`Python.Python.3.12`).
3. **Workspace:** avisa se a pasta pai tiver outros projetos.
4. **venv** em `<workspace>\.venv` + `pip install west`. A ativação é feita
   pelas variáveis de ambiente, sem depender da ExecutionPolicy.
5. **Zephyr:**
   - `west init -l <repo>`
   - `west update --narrow --fetch-opt=--depth=1`
   - `west zephyr-export`
   - dependências Python via `west packages pip`
6. **SDK:** `west sdk install -t arm-zephyr-eabi` em `~\zephyr-sdk-1.0.1`.
   O 7-Zip extrai o pacote.
7. **J-Link:** localiza a instalação pelo registro
   (`HKCU\Software\SEGGER\J-Link`) e alerta se o `JLink.exe` do PATH for o
   `jlink.exe` do JDK.
8. **Build de teste** de `tag`, `anchor` e `gateway`.

### 2.3 Uso diário

Em cada terminal novo:

```powershell
. .\scripts\env.ps1
```

Se a ExecutionPolicy bloquear o `env.ps1`, libere uma vez para o seu usuário:

```powershell
Set-ExecutionPolicy -Scope CurrentUser RemoteSigned
```

Ou abra um shell já ativado:

```powershell
powershell -ExecutionPolicy Bypass -NoExit -File .\scripts\env.ps1
```

O `env.ps1` ativa o venv e coloca o J-Link da SEGGER na frente do PATH.

### 2.4 WSL2 (opcional)

O WSL2 com Ubuntu 24.04 serve para rodar os **testes unitários em
`native_sim`**, que não roda no Windows nativo. Siga a seção 3 dentro do WSL.
Para gravar a partir do WSL é preciso encaminhar o USB com o
[`usbipd-win`](https://github.com/dorssel/usbipd-win). Por isso é mais
simples gravar pelo Windows.

---

## 3. Linux (Ubuntu/Debian)

### 3.1 Pré-requisitos

- Ubuntu **24.04+** ou Debian 12+ (Python ≥ 3.12). No Ubuntu 22.04, instale
  o Python 3.12 pelo PPA *deadsnakes* antes.
- **SEGGER J-Link** (.deb), baixado em <https://www.segger.com/downloads/jlink/>:

  ```bash
  sudo apt install ./JLink_Linux_V*_x86_64.deb
  ```

  O pacote instala também as regras udev.

### 3.2 Setup automático

```bash
./scripts/setup/setup-linux.sh
```

| Opção | Efeito |
|-------|--------|
| `-y`, `--yes` | não pergunta nada |
| `--skip-packages` | não usa o apt |
| `--skip-sdk` | não instala o Zephyr SDK |
| `--skip-build` | pula o build de teste |
| `--full-clone` | histórico completo do Zephyr |

Passos:

1. **apt:** `git cmake ninja-build gperf ccache dfu-util device-tree-compiler
   wget python3-dev python3-venv python3-tk xz-utils file make gcc libsdl2-dev
   libmagic1`, mais `gcc-multilib g++-multilib` em x86_64 (usados pelo
   `native_sim`).
2. Python ≥ 3.12 (`python3`, `python3.13` ou `python3.12`).
3. Aviso sobre a pasta do workspace.
4. venv + west. Se o CMake do sistema for antigo, instala `cmake` via pip no venv.
5. `west init -l`, `west update` (raso), `west zephyr-export`,
   `west packages pip --install`.
6. `west sdk install -t arm-zephyr-eabi` (inclui as *host tools*).
7. Oferece adicionar o usuário ao grupo `dialout` (acesso a `/dev/ttyACM*`,
   vale após novo login) e verifica o J-Link.
8. Build de teste dos três apps.

### 3.3 Uso diário

```bash
source scripts/env.sh
```

---

## 4. Passo a passo manual: Windows

Equivale ao script. Use para entender o processo ou diagnosticar falhas.

```powershell
# 1. Ferramentas
winget install Kitware.CMake Ninja-build.Ninja oss-winget.gperf oss-winget.dtc Git.Git 7zip.7zip
pymanager install 3.12          # ou: winget install Python.Python.3.12
# feche e reabra o terminal; se preciso: $env:Path += ";$env:ProgramFiles\7-Zip"

# 2. venv (na pasta do workspace, ex.: C:\Workspace\rtls-ws)
cd C:\Workspace\rtls-ws
py -3.12 -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install west

# 3. Zephyr + módulos
west init -l dwm1001-rtls-fw
cd dwm1001-rtls-fw
west update --narrow --fetch-opt=--depth=1
west zephyr-export
python -m pip install @((west packages pip) -split ' ')

# 4. SDK
cd ..\zephyr
west sdk install -t arm-zephyr-eabi

# 5. Teste
cd ..\dwm1001-rtls-fw
west build -b decawave_dwm1001_dev apps/tag -d build/tag
```

## 5. Passo a passo manual: Linux

```bash
sudo apt install --no-install-recommends git cmake ninja-build gperf ccache dfu-util \
  device-tree-compiler wget python3-dev python3-venv python3-tk xz-utils file make gcc \
  gcc-multilib g++-multilib libsdl2-dev libmagic1

cd ~/rtls-ws
python3 -m venv .venv
source .venv/bin/activate
pip install west

west init -l dwm1001-rtls-fw
cd dwm1001-rtls-fw
west update --narrow --fetch-opt=--depth=1
west zephyr-export
west packages pip --install

(cd ../zephyr && west sdk install -t arm-zephyr-eabi)

west build -b decawave_dwm1001_dev apps/tag -d build/tag
```

---

## 6. Build, gravação e monitor

Os comandos valem para os dois sistemas, com o ambiente ativo e a partir da
raiz do repositório.

### 6.1 Build

```bash
# Tag
west build -b decawave_dwm1001_dev apps/tag -d build/tag

# Âncoras: uma pasta de build por âncora, com o fragmento de configuração dela
west build -b decawave_dwm1001_dev apps/anchor -d build/anchor_a1 -- "-DEXTRA_CONF_FILE=nodes/a1.conf"
west build -b decawave_dwm1001_dev apps/anchor -d build/anchor_a2 -- "-DEXTRA_CONF_FILE=nodes/a2.conf"

# Gateway
west build -b decawave_dwm1001_dev apps/gateway -d build/gateway
```

> [!IMPORTANT]
> Mantenha as aspas em `"-DEXTRA_CONF_FILE=..."`. Sem elas, o Windows
> PowerShell 5.1 corta o argumento no ponto e o CMake procura `nodes/a1` (ver
> §7).

Comandos úteis:

| Comando | Para quê |
|---------|----------|
| `west build -d build/tag -p always ...` | build limpo (*pristine*) |
| `west build -d build/tag -t menuconfig` | explorar o Kconfig |
| `west build -d build/tag -t ram_report` / `-t rom_report` | uso de memória |

### 6.2 Gravação

O J-Link OB da placa é usado pelo runner `jlink`, o padrão da placa.

```bash
west flash -d build/tag
```

Com **várias placas conectadas**, escolha a placa pelo serial do J-Link:

```powershell
.\scripts\list-boards.ps1                 # Windows: porta COM x serial
```

```bash
scripts/list-boards.sh                    # Linux: /dev/ttyACMx x serial
```

```bash
west flash -d build/anchor_a2 --dev-id 760123456
```

Dica: anote o serial de cada placa numa etiqueta (A1..A4, T1, GW).

### 6.3 Console (logs)

UART0 pela porta COM virtual do J-Link, a **115200 8N1**:

```powershell
python -m serial.tools.miniterm COM5 115200    # Windows (pyserial já vem no venv)
```

```bash
python -m serial.tools.miniterm /dev/ttyACM0 115200    # Linux
```

Também servem o PuTTY, o Tera Term ou o monitor serial do VS Code. No boot da
tag, o esperado no M0 é:

```
<inf> tag: RTLS tag 0x1001: 10 Hz, 4 ancoras a partir de 0xa001
<inf> rtls_dw1000: DW1000 DEV_ID 0xdeca0130 part 0x........ xtal 16 ldo 0x.. otp_antdly 0x........
<inf> rtls_dw1000: PHY: canal 5, PRF 64 MHz, preambulo 128 (codigo 9), 6800 kbps, PAN 0xdeca, addr 0x1001
```

O boot esperado de cada papel e os comandos de bancada estão em
[08 §2](08-etapas-e-testes.md#2-convenções-e-guia-de-bancada).

### 6.4 Debug

```bash
west debug -d build/tag        # GDB + J-Link GDB Server
west attach -d build/tag       # anexa sem regravar
```

No VS Code, a extensão *Cortex-Debug* com `servertype: jlink`, device
`nRF52832_xxAA` e o ELF em `build/tag/zephyr/zephyr.elf`.

### 6.5 Testes unitários

Rodam em `native_sim`, que exige Linux ou WSL:

```bash
west twister -T tests -p native_sim
```

### 6.6 Bancada (M1 a M3)

O app de teste do driver (`samples/uwb_pingpong`), a captura do CSV e a
calibração de antena (`tools/rtls_log.py`) estão descritos em
[07-bancada.md](07-bancada.md).

---

## 7. Solução de problemas

| Sintoma | Causa provável | Solução |
|---------|----------------|---------|
| `west: command not found` / `'west' não é reconhecido` | venv não ativado | `. .\scripts\env.ps1` ou `source scripts/env.sh` |
| `west flash` usa o `jlink.exe` do Java ("Could not load J-Link library") | JDK antes do SEGGER no PATH e sem chave de registro | Ative o `env.ps1` ou use `west flash --commander "C:\Program Files\SEGGER\JLink_V986a\JLink.exe"` |
| `west sdk install` falha ao extrair no Windows | 7-Zip fora do PATH | `$env:Path += ";$env:ProgramFiles\7-Zip"` |
| `Could not find a package configuration file provided by "Zephyr-sdk"` | SDK não registrado no CMake | rode `setup.cmd /c` (Windows) ou `setup.sh -c` (Linux) dentro de `~/zephyr-sdk-1.0.1` |
| Erros de caminho longo no Windows | limite de 260 caracteres | workspace com caminho curto (ex.: `C:\ws`); `git config --global core.longpaths true` |
| `Permission denied: /dev/ttyACM0` (Linux) | usuário fora do `dialout` | `sudo usermod -aG dialout $USER` e novo login |
| A porta serial some ou fica ocupada no Linux | ModemManager capturando o ttyACM | `sudo systemctl disable --now ModemManager` |
| `DEV_ID inesperado 0xffffffff` / `0x00000000` | SPI sem resposta (DW1000 em reset ou sem alimentação) | conferir a alimentação da placa; testar com outra placa |
| `native_sim` não compila no Windows | não suportado no Windows | use o WSL2 ou a CI |
| `Ignoring extra path from command line: ".conf"` e `File not found: .../apps/anchor/nodes/a1` | o Windows PowerShell 5.1 cortou `-DEXTRA_CONF_FILE=nodes/a1.conf` no ponto | escreva a opção entre aspas (`"-DEXTRA_CONF_FILE=nodes/a1.conf"`) e refaça com `-p always` |
| winget: `Failed to extract the contents of the archive` | extração de pacotes .zip bloqueada (antivírus/política) | o script continua e instala CMake/Ninja via pip; para gperf/dtc, instale manualmente se precisar |
| Build lento no Windows | antivírus varrendo o workspace | avalie uma exclusão do Defender para a pasta do workspace (decisão sua; afeta a segurança) |

## 8. Atualizar o Zephyr no futuro

1. Altere `revision:` no [`west.yml`](../west.yml).
2. Rode `west update` e, em seguida, `west packages pip --install`.
3. Instale o SDK da nova versão com `west sdk install -t arm-zephyr-eabi`.
4. Leia o guia de migração da nova versão.
5. Rode `west build -p always` dos três apps e os testes.

Faça isso entre marcos, nunca no meio de uma campanha de medição do TCC.

# dwm1001-rtls-fw

Firmware de um **RTLS por UWB** (Two-Way Ranging) com algoritmo próprio para
a placa **Qorvo/Decawave DWM1001-DEV** (nRF52832 + DW1000), sobre o
**Zephyr RTOS v4.4.0**. Projeto de TCC.

```
 Âncoras (4, fixas) <── DS-TWR ──> Tag (10 Hz, EKF) ── POSITION ──> Gateway ── SPI ──> Host ──> rede
```

| Papel | Diretório | Função |
|-------|-----------|--------|
| **Tag** | [`apps/tag`](apps/tag) | Mede a distância até 4 âncoras por DS-TWR, estima a posição com filtro de Kalman e envia ao gateway |
| **Âncora** | [`apps/anchor`](apps/anchor) | Nó fixo: responde ao DS-TWR e informa a própria posição |
| **Gateway** | [`apps/gateway`](apps/gateway) | Recebe as posições das tags e as entrega a um host via SPI escravo |

## Estado

**M0 (estrutura base) concluído:**
- esqueleto dos três firmwares;
- probe do DW1000;
- matemática do DS-TWR e codec de quadros, com testes;
- scripts de setup.

Os próximos marcos estão em
[docs/01-planejamento.md](docs/01-planejamento.md#6-roadmap-marcos). Os pontos
a implementar estão marcados no código como `TODO(Mx)`.

## Início rápido

```powershell
# Windows (PowerShell)
powershell -ExecutionPolicy Bypass -File .\scripts\setup\setup-windows.ps1
. .\scripts\env.ps1
```

```bash
# Linux (Ubuntu 24.04+)
./scripts/setup/setup-linux.sh
source scripts/env.sh
```

Depois:

```bash
west build -b decawave_dwm1001_dev apps/tag -d build/tag
west flash -d build/tag
```

Guia completo, com passo a passo manual e solução de problemas:
[docs/06-configuracao-ambiente.md](docs/06-configuracao-ambiente.md).

## Documentação

| Documento | Conteúdo |
|-----------|----------|
| [01-planejamento](docs/01-planejamento.md) | Escopo, requisitos, decisões, roadmap M0–M8, validação, riscos |
| [02-arquitetura](docs/02-arquitetura.md) | Hardware, pinos, camadas, papéis, threads, Kconfig |
| [03-protocolo-uwb](docs/03-protocolo-uwb.md) | PHY, quadros, sequência DS-TWR, temporização, TDMA |
| [04-algoritmos](docs/04-algoritmos.md) | DS-TWR, calibração de antena, multilateração, EKF |
| [05-interface-spi-gateway](docs/05-interface-spi-gateway.md) | Protocolo SPI gateway → host |
| [06-configuracao-ambiente](docs/06-configuracao-ambiente.md) | Setup Windows/Linux, build, gravação, debug |

## Estrutura

```
apps/{tag,anchor,gateway}   firmwares por papel
drivers/dw1000              driver próprio do DW1000 (timestamps de 40 bits)
include/rtls                APIs públicas
lib/{twr,loc,proto,host_if,common}
tests/unit                  ztest (native_sim)
scripts/                    setup, ativação do ambiente, lista de placas
docs/                       planejamento e documentação técnica
west.yml                    Zephyr v4.4.0 + módulos mínimos
```

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

| Marco | Situação |
|-------|----------|
| M0: ambiente e estrutura | concluído |
| M1: driver DW1000 próprio | implementado; validar em bancada com `samples/uwb_pingpong` |
| M2: DS-TWR 1×1 + calibração | implementado, com correção de viés por RSL (APS011); validar e calibrar com `tools/rtls_log.py` |
| M3: DS-TWR 1×4 a 10 Hz | implementado; validar em bancada |
| M4: localização (multilateração + EKF) | implementado; testes em simulação aprovados na placa; validar em bancada (grade de pontos, tempo ≤ 1 ms) |
| M5: posição tag → gateway | implementado: CSV no gateway, lacunas por tag e `rtls_log.py gw`; validar a entrega em bancada (≥ 99 %) |
| M6: SPI → host | implementado: codec com CRC testado, thread SPIS + DRDY, tags simuladas para teste; validar com um host real |
| M7: TDMA | implementado: BEACON do gateway e slots por ID na tag (opções `RTLS_GATEWAY_BEACON` e `RTLS_TAG_TDMA`); validar com 2 a 4 tags |
| M8: robustez | implementado: parâmetros na NVS pelo shell (`rtls set`), watchdog, indicador de NLOS e ponderação opcional no EKF; falta a campanha experimental |
| Infraestrutura | CI no GitHub Actions (builds + `native_sim`), GPIO de debug para o analisador lógico, reprocessamento offline das capturas (`rtls_log.py replay`) |
| M9 e M10 (opcionais) | se houver tempo: descoberta e seleção de âncoras; até 8 âncoras por rodada com detecção de NLOS por redundância |
| M11 e M12 (opcionais) | se houver tempo: modo SS-TWR escolhido por *flag*, com compensação do desvio de relógio; configuração remota de tags e âncoras pelo host, via gateway |

As etapas, com roteiro de teste e critério de aceite, estão em
[docs/08-etapas-e-testes.md](docs/08-etapas-e-testes.md); o roadmap e as
decisões, em [docs/01-planejamento.md](docs/01-planejamento.md#6-roadmap-marcos).
Os pontos ainda não implementados estão marcados no código como `TODO(Mx)`.

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
| [01-planejamento](docs/01-planejamento.md) | Escopo, requisitos, decisões, roadmap M0–M8 (+ M9–M12 opcionais), validação, riscos |
| [02-arquitetura](docs/02-arquitetura.md) | Hardware, pinos, camadas, papéis, threads, Kconfig |
| [03-protocolo-uwb](docs/03-protocolo-uwb.md) | PHY, quadros, sequência DS-TWR, temporização, TDMA; extensões M9–M12 |
| [04-algoritmos](docs/04-algoritmos.md) | DS-TWR (e SS-TWR, M11), calibração de antena, multilateração, EKF |
| [05-interface-spi-gateway](docs/05-interface-spi-gateway.md) | Protocolo SPI gateway → host (e comandos de configuração do M12) |
| [06-configuracao-ambiente](docs/06-configuracao-ambiente.md) | Setup Windows/Linux, build, gravação, debug |
| [07-bancada](docs/07-bancada.md) | Roteiro de validação em hardware do M1 ao M3, calibração de antena |
| [08-etapas-e-testes](docs/08-etapas-e-testes.md) | Todas as etapas (M0–M8 e os opcionais M9–M12) com testes automatizados, roteiro de bancada e critério de aceite |
| [09-guia-do-codigo](docs/09-guia-do-codigo.md) | **Guia vivo do código**: arquitetura com diagramas, cada componente, Zephyr no projeto e todos os parâmetros editáveis |

## Estrutura

```
apps/{tag,anchor,gateway}   firmwares por papel
drivers/dw1000              driver próprio do DW1000 (timestamps de 40 bits)
include/rtls                APIs públicas
lib/{twr,loc,proto,host_if,common}
samples/uwb_pingpong        bancada do driver (M1)
tests/unit                  ztest (native_sim)
tools/rtls_log.py           captura do CSV, estatísticas e calibração de antena
scripts/                    setup, ativação do ambiente, lista de placas
docs/                       planejamento e documentação técnica
west.yml                    Zephyr v4.4.0 + módulos mínimos
```

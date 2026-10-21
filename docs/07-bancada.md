# 07 — Testes de bancada (M1 a M3)

Roteiro para validar em hardware os marcos M1 (driver), M2 (DS-TWR 1×1 e
calibração) e M3 (DS-TWR 1×4 a 10 Hz). Os critérios de aceite são os de
[01-planejamento.md §6](01-planejamento.md#6-roadmap-marcos).

Todos os comandos partem da raiz do repositório, com o ambiente ativo
(`. .\scripts\env.ps1` ou `source scripts/env.sh`).

## 0. Preparação

1. **Identifique as placas.** Rode `.\scripts\list-boards.ps1` (Windows) ou
   `scripts/list-boards.sh` (Linux) com todas conectadas e cole uma etiqueta
   em cada uma, com papel e serial do J-Link (ex.: `T1 760012345`).
2. **Grave sempre com `--dev-id`:** `west flash -d <build> --dev-id <serial>`.
3. **Console:** `python -m serial.tools.miniterm COMx 115200`. Cada placa tem
   a própria porta COM virtual.
4. **Ambiente:**
   - linha de visada (LOS);
   - placas na mesma altura (≥ 1 m do chão);
   - antenas na mesma orientação;
   - distâncias medidas com trena laser entre os centros dos módulos.

## 1. M1: driver DW1000 (ping-pong)

O app [`samples/uwb_pingpong`](../samples/uwb_pingpong) exercita TX imediato,
TX atrasado, RX automático após o TX, IRQ e os diagnósticos de potência.

```bash
west build -b decawave_dwm1001_dev samples/uwb_pingpong -d build/ping
west build -b decawave_dwm1001_dev samples/uwb_pingpong -d build/pong -- -DCONFIG_PINGPONG_ROLE_PONG=y
west flash -d build/ping --dev-id <serial A>
west flash -d build/pong --dev-id <serial B>
```

Log esperado ao ligar cada placa:

```
<inf> rtls_dw1000: DW1000 DEV_ID 0xdeca0130 part 0x........ xtal 16 ldo 0x.. otp_antdly 0x........
<inf> rtls_dw1000: PHY: canal 5, PRF 64 MHz, preambulo 128 (codigo 9), 6800 kbps, PAN 0xdeca, addr 0x0f01
```

A cada 100 pings (10 s):

```
<inf> pingpong: ping: enviados 600, pongs 598, PER 0.33%, RX medio -7950 cdBm, erros de TX 0
<inf> pingpong: pong: respondidos 600, TX atrasado perdido 0, RX ultimo -7962 cdBm (1o caminho -8210)
```

**Aceite do M1:** a 3 m, por 10 min (6000 pings), PER < 1 % e
`TX atrasado perdido 0`.

| Sintoma | Verifique |
|---------|-----------|
| `DEV_ID inesperado` | alimentação; o DW1000 não sai do reset |
| `PLL do DW1000 nao travou` | fonte/USB fraca; repita com outra porta USB |
| `TX nao terminou` | IRQ (P0.19); o TX não completou no prazo |
| PER de 100 % | as duas placas ligadas? papéis diferentes (`ping`/`pong`)? |
| `TX atrasado perdido` > 0 | aumente `CONFIG_PINGPONG_REPLY_US` |

## 2. M2: DS-TWR 1×1 e calibração de antena

### 2.1 Medir

Uma tag consultando uma âncora:

```bash
west build -b decawave_dwm1001_dev apps/tag -d build/tag1 -- -DCONFIG_RTLS_TAG_NUM_ANCHORS=1
west build -b decawave_dwm1001_dev apps/anchor -d build/anchor_a1 -- -DEXTRA_CONF_FILE=nodes/a1.conf
west flash -d build/tag1 --dev-id <serial tag>
west flash -d build/anchor_a1 --dev-id <serial ancora>
```

A tag imprime uma linha CSV por distância:

```
R,<round>,<t_ms>,<ancora>,<dist_mm>,<rx_power_cdbm>,<fp_power_cdbm>
```

Ela também imprime um resumo a cada 10 s. Para capturar 100 s (1000 medidas)
e analisar, com o console fechado:

```bash
python tools/rtls_log.py capture --port COM5 --out m2_3m.csv --seconds 100
python tools/rtls_log.py stats m2_3m.csv --true-distance 3.0
```

### 2.2 Calibrar (Decawave APS014; teoria em [04-algoritmos.md §3](04-algoritmos.md#3-calibração-do-atraso-de-antena))

**Par de placas iguais** (rápido, assume atrasos iguais):

```bash
python tools/rtls_log.py calib m2_3m.csv --true-distance 3.0
# ambas: delta +10.0 ticks -> CONFIG_RTLS_DW1000_TX_ANT_DLY=16441  CONFIG_RTLS_DW1000_RX_ANT_DLY=16441
```

**Três placas A, B e C** (valor individual por placa):

1. Grave A como tag (`-DCONFIG_RTLS_TAG_NUM_ANCHORS=2`), B como âncora
   `a1.conf` e C como âncora `a2.conf`. Assim os pares A-B e A-C saem na
   mesma captura. Use `--anchor a001` e `--anchor a002` para separar.
2. Grave B como tag e C como âncora `a1.conf` e capture o par B-C.
3. Rode:

```bash
python tools/rtls_log.py calib3 --true-distance 3.0 --ab ab.csv --ac ac.csv --bc bc.csv
```

Coloque os valores de cada placa no fragmento de configuração dela. Para
âncoras, acrescente as duas linhas em `apps/anchor/nodes/aN.conf`. Para a
tag, crie um fragmento próprio, por exemplo `apps/tag/nodes/t1.conf`, e use
`-DEXTRA_CONF_FILE=nodes/t1.conf`.

**Aceite do M2:** a 1, 3 e 5 m, |viés| ≤ 10 cm e desvio ≤ 5 cm (1000
medidas por distância).

## 3. M3: DS-TWR 1×4 a 10 Hz

1. Meça as coordenadas das 4 âncoras no referencial da sala e atualize
   `apps/anchor/nodes/a1..a4.conf`, junto com os atrasos de antena calibrados.
2. Grave:

   ```bash
   west build -b decawave_dwm1001_dev apps/anchor -d build/anchor_a1 -- -DEXTRA_CONF_FILE=nodes/a1.conf
   # ... a2, a3, a4
   west build -b decawave_dwm1001_dev apps/tag -d build/tag
   ```

3. Acompanhe os resumos. Na tag:

   ```
   <inf> tag: TWR: 100 rodadas, 98% completas, ultima 9460 us, max 9512 us, TX atrasado 0
   <inf> tag:   ancora 0xa001: RESP 100, distancias 100 (100%), invalidas 0
   ```

   Em cada âncora, a cada 10 s:

   ```
   <inf> anchor: TWR: POLL 100, RESP 100, FINAL 99 (timeout 1), REPORT 99, TX atrasado 0
   ```

4. Capture 10 min e confira a taxa e o sucesso por âncora:

   ```bash
   python tools/rtls_log.py capture --port COM5 --out m3.csv --seconds 600
   python tools/rtls_log.py stats m3.csv
   ```

**Aceite do M3:**
- ≥ 95 % de sucesso em cada âncora;
- taxa de 10,00 Hz no `stats`;
- `max` da rodada ≤ 12 ms;
- `TX atrasado 0` em todos os nós.

### 3.1 Ajuste fino da temporização

| Situação | Ação |
|----------|------|
| `TX atrasado` > 0 na tag ou nas âncoras | aumente `CONFIG_RTLS_TWR_REPLY_DELAY_US` (tag e âncoras iguais) |
| RESP ok mas REPORT falhando | aumente `CONFIG_RTLS_TWR_RX_MARGIN_US` |
| Tudo estável | reduza `REPLY_DELAY_US`/`SLOT_US` aos poucos (ex.: 800 → 600 µs) para encurtar a rodada; abaixo de ~500 µs a latência do Zephyr começa a perder slots |

> `REPLY_DELAY_US` e `SLOT_US` precisam ser **iguais na tag e em todas as
> âncoras**: passe os mesmos `-DCONFIG_...` em todos os builds.

## 4. Registro para o TCC

Guarde os CSVs brutos fora do git (`captures/` está no `.gitignore`).
Registre também as condições de cada ensaio:

| Data | Marco | Distância/cenário | n | Viés (mm) | Desvio (mm) | Sucesso | Observações |
|------|-------|-------------------|---|-----------|-------------|---------|-------------|
| | | | | | | | |

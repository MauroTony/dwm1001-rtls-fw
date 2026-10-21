# 05 — Interface SPI gateway → host

O gateway funciona como **SPI escravo**. O host externo ("a definir": ESP32-S3,
Raspberry Pi etc.) é o mestre: lê a tabela de posições e a envia para a rede.

O protocolo copia o modelo de transação **TLV-sobre-SPI do PANS**: request
TLV, *polling* do meta-header `[SIZE][NUM]`, leitura da resposta e
recuperação por `0xFF`. O que muda são os opcodes, próprios do RTLS. Assim o
host ESP32-S3 do repositório `dwm1001-gateway-esp` reaproveita a HAL
(`SpiHal` com CS manual, `GpioController` para IRQ/RESET) e o `TlvCodec`. Só
a camada de comandos é nova.

## 1. Camada física

| Sinal | Pino nRF52832 | Direção | Observação |
|-------|---------------|---------|------------|
| SCK | P0.04 | host → gw | modo 0 (CPOL=0, CPHA=0), ≤ 8 MHz, MSB primeiro |
| MOSI | P0.06 | host → gw | |
| MISO | P0.07 | gw → host | |
| CSn | P0.03 | host → gw | ativo baixo; 1 transação = 1 pulso de CS |
| DRDY | P0.26 | gw → host | ativo alto: há um *snapshot* novo não lido |
| nRESET | P0.21 | host → gw | opcional; pulso baixo ≥ 1 ms reinicia o gateway |
| GND | — | — | referência comum obrigatória |

Restrições do SPIS do nRF52832:

- No máximo **255 bytes por transação** (EasyDMA com `MAXCNT` de 8 bits). Por
  isso a tabela é **paginada**.
- Entre transações o firmware precisa rearmar os buffers. O host deve
  esperar **≥ 50 µs** entre pulsos de CS. Enquanto o gateway não rearma, o
  SPIS devolve o caractere padrão `0x00` (`def-char` no devicetree), que o
  host interpreta como "ainda não pronto".

## 2. Modelo de transação

```mermaid
sequenceDiagram
    participant H as Host (mestre)
    participant G as Gateway (SPIS)
    H->>G: CS↓ [TYPE][LEN][VALUE…] CS↑   (request)
    loop até SIZE ≠ 0 (timeout 10 ms)
        H->>G: CS↓ lê 2 bytes (envia 0xFF) CS↑
        G-->>H: [SIZE][NUM]  ou  [0x00][0x00] (processando)
    end
    H->>G: CS↓ lê SIZE×NUM bytes (envia 0xFF) CS↑
    G-->>H: [0x40][0x01][status] + TLV de resposta (opcional)
```

- **Envelope de resposta:** toda resposta começa com `[0x40][0x01][status]`.
  Se o comando devolve dados, segue um segundo TLV `[tipo][len][valor]`.
- **Status:** `0` OK · `1` comando desconhecido · `2` parâmetro inválido ·
  `3` ocupado.
- **Recuperação:** três transações de 1 byte `0xFF` (NOP) devolvem o gateway
  ao estado IDLE, como no PANS.
- **TLV:** `type:u8`, `len:u8` (≤ 253), `value[len]`. Campos multibyte são
  little-endian.

## 3. Comandos

| Request | Nome | Value | Resposta |
|---------|------|-------|----------|
| `0xA0` | `RTLS_INFO_GET` | — | envelope + `0xB0` (11 B) |
| `0xA1` | `RTLS_TAGS_GET` | `page:u8` | envelope + `0xB1` (4 + 26·n + 2 B) |
| `0xA2` | `RTLS_DRDY_CFG_SET` | `enable:u8` | envelope |
| `0xFF` | NOP | — | — |

### 3.1 `0xB0`: informações do gateway (11 bytes)

| Offset | Tipo | Campo |
|--------|------|-------|
| 0 | u8 | versão do protocolo (`1`) |
| 1 | u8[3] | versão do firmware (major, minor, patch) |
| 4 | u16 | ID do gateway |
| 6 | u8 | capacidade da tabela (`CONFIG_RTLS_GATEWAY_MAX_TAGS`) |
| 7 | u8 | tags ativas |
| 8 | u16 | período do superquadro (ms) |
| 10 | u8 | registros por página (`9`) |

### 3.2 `0xB1`: página da tabela de tags

| Offset | Tipo | Campo |
|--------|------|-------|
| 0 | u8 | `page` (0..n_pages−1) |
| 1 | u8 | `n_pages` |
| 2 | u8 | `count`: registros nesta página (≤ 9) |
| 3 | u8 | `snapshot`: sequência do snapshot; todas as páginas do mesmo snapshot têm o mesmo valor |
| 4 | rec[count] | registros de 26 B (abaixo) |
| 4+26·count | u16 | CRC-16/CCITT-FALSE (poli `0x1021`, init `0xFFFF`) sobre os bytes anteriores do *value* |

Com 9 registros, o *value* tem 240 B e a resposta inteira tem
3 + 2 + 240 = **245 B**, ou seja, uma única transação com `NUM = 1`.

### 3.3 Registro de tag (26 bytes, `struct rtls_tag_record`)

| Offset | Tipo | Campo |
|--------|------|-------|
| 0 | u16 | `tag_id` |
| 2 | u8 | `round`: sequência da última rodada recebida |
| 3 | u8 | `n_ranges`: distâncias usadas na estimativa |
| 4 | u8 | `flags`: os mesmos do quadro POSITION |
| 5 | i8 | `rssi_dbm` do quadro POSITION no gateway |
| 6 | u16 | `age_ms`: idade da posição no instante do snapshot |
| 8 | i32[3] | `pos_mm`: x, y, z |
| 20 | u16[3] | `std_mm`: σx, σy, σz |

## 4. Sinal DRDY

- O gateway gera um *snapshot* da tabela no máximo a cada 100 ms, e só se
  algo mudou.
- O DRDY **sobe** quando há um snapshot novo e **desce** quando o host lê a
  última página dele.
- O host pode trabalhar por interrupção (borda de subida) ou por polling
  periódico com `RTLS_TAGS_GET`.
- `RTLS_DRDY_CFG_SET(0)` desliga o sinal (fica sempre baixo).

## 5. Fluxo típico no host

```text
ao detectar borda de subida no DRDY:
    page = 0
    repete:
        transação: [0xA1][0x01][page]
        (size, num) = poll_meta_header()          # repete enquanto (0,0)
        resp = lê size*num bytes
        valida envelope 0x40 (status 0), TLV 0xB1 e CRC
        se snapshot mudou no meio da leitura: recomeça com page = 0
        publica os registros (ex.: MQTT)
        page += 1
    até page == n_pages
```

## 6. Implementação no gateway (M6)

- O nó `spi1` do devicetree vira `nordic,nrf-spis` em
  [`apps/gateway/boards/decawave_dwm1001_dev.overlay`](../apps/gateway/boards/decawave_dwm1001_dev.overlay).
  O DRDY fica em `zephyr,user/drdy-gpios`.
- A thread `host_if` faz um laço de `spi_transceive()` em modo escravo, que
  bloqueia até o CS subir. Cada chamada corresponde a uma transação e à
  próxima etapa da máquina de estados (IDLE → REQ → META → DATA → IDLE).
- O snapshot vem de `rtls_host_if_publish()` com cópia sob mutex. O host
  sempre lê um estado coerente.

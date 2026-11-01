# 04 — Algoritmos

Esta é a base matemática do que roda na tag. A matemática do DS-TWR já está
implementada em [`lib/twr/twr_math.c`](../lib/twr/twr_math.c), com testes. A
multilateração e o filtro de Kalman, núcleo do TCC, estão especificados aqui
e entram no marco M4 ([`lib/loc/loc.c`](../lib/loc/loc.c)).

## 1. Timestamps do DW1000

- Resolução: $t_u = \dfrac{1}{128 \cdot 499{,}2\,\text{MHz}} \approx 15{,}65\,\text{ps}$,
  o que equivale a $c \cdot t_u \approx 4{,}69\,\text{mm}$ por tick.
- O contador tem **40 bits** e dá a volta a cada $2^{40} t_u \approx 17{,}2\,\text{s}$.
  Todo intervalo é calculado módulo $2^{40}$:
  $\Delta = (t_{fim} - t_{inicio}) \bmod 2^{40}$ (`rtls_dw_ts_diff`).
- Os timestamps de RX e TX já descontam os atrasos de antena programados nos
  registradores `LDE_RXANTD` e `TX_ANTD`.

## 2. DS-TWR assimétrico

Intervalos de uma rodada entre a tag (A) e a âncora (B), cada um medido no
relógio do próprio nó:

| Símbolo | Definição | Relógio |
|---------|-----------|---------|
| $R_a$ | `resp_rx − poll_tx` | tag |
| $D_a$ | `final_tx − resp_rx` | tag |
| $R_b$ | `final_rx − resp_tx` | âncora |
| $D_b$ | `resp_tx − poll_rx` | âncora |

Estimador (Neirynck et al., 2016):

$$
\widehat{ToF} = \frac{R_a R_b - D_a D_b}{R_a + R_b + D_a + D_b},
\qquad d = c_{ar} \cdot \widehat{ToF} \cdot t_u
$$

com $c_{ar} = 299\,702\,547\ \text{m/s}$.

**Por que é robusto à deriva de relógio:** se os relógios têm erros relativos
$e_A$ e $e_B$, então

$$
\widehat{ToF} = ToF \cdot \frac{2(1+e_A)(1+e_B)}{2 + e_A + e_B} \approx ToF\left(1 + \frac{e_A+e_B}{2}\right)
$$

O erro é proporcional ao **ToF** (nanossegundos), não aos tempos de resposta
(milissegundos). Com ±20 ppm, o erro fica em ~0,2 mm a 10 m. Os tempos de
resposta também não precisam ser iguais ($D_b \ne D_a$), e isso permite os
slots diferentes de cada âncora. O teste
`tests/unit/src/test_twr_math.c` verifica exatamente esse caso.

**Implementação:** os produtos $R_a R_b$ chegam a ~$10^{17}$ (intervalos de
~5 ms ≈ $3\cdot10^8$ ticks). Por isso o cálculo usa `double`. O erro de
arredondamento resultante fica na ordem de $10^{-8}$ tick.

**Viés dependente da potência:** o DW1000 tem um viés de range que varia com
o nível de sinal recebido (Decawave APS011). A correção por tabela está
ligada desde o M2 ([§3.1](#31-correção-do-viés-dependente-do-rsl-aps011)).

### 2.1 SS-TWR com compensação do desvio (M11, opcional)

> **Opcional, a implementar.** Protocolo em
> [03 §8.1](03-protocolo-uwb.md#81-ss-twr-m11); fundamentação em `tcc/04` §9.

Só a primeira perna (POLL → RESP_SS). Sem compensação, o erro é o do
APS011:

$$
\widehat{ToF}_{SS} = \frac{R_a - D_b}{2},
\qquad
\widehat{ToF}_{SS} - ToF \approx \frac{D_b\,(e_A - e_B)}{2}
$$

**Estimador compensado:** com $\varepsilon \approx e_B - e_A$ (frequência da
âncora em relação à da tag), o $D_b$ é levado ao relógio da tag:

$$
\widehat{ToF} = \frac{R_a - D_b\,(1 - \hat\varepsilon)}{2},
\qquad
\text{erro} \approx \frac{D_b\,(\hat\varepsilon - \varepsilon)}{2}
$$

| Símbolo | Significado |
|---------|-------------|
| $R_a$ | `resp_rx − poll_tx`, relógio da tag |
| $D_b$ | `resp_tx − poll_rx`, relógio da âncora (`resp_tx` previsto) |
| $e_A$, $e_B$ | erro de frequência da tag e da âncora |
| $\varepsilon$, $\hat\varepsilon$ | desvio relativo real e estimado |
| $D_{b,i} = T_{reply} + i\,T_{slot}$ | tempo de resposta da âncora no slot $i$ |

**Orçamento de erro:** $\tfrac12 D_b\,\delta\varepsilon\,c$ dá ~15 cm por ms
e por ppm de erro na estimativa.

| $\delta\varepsilon$ | Slot 0 (1 ms) | Slot 1 (2 ms) | Slot 2 (3 ms) | Slot 3 (4 ms) |
|---------------------|---------------|---------------|---------------|---------------|
| 2 ppm (sem compensação, exemplo) | 30 cm | 60 cm | 90 cm | 120 cm |
| 1 ppm | 15 cm | 30 cm | 45 cm | 60 cm |
| 0,1 ppm | 1,5 cm | 3,0 cm | 4,5 cm | 6,0 cm |
| 10 ppb | 1,5 mm | 3,0 mm | 4,5 mm | 6,0 mm |

Para ficar dentro do critério do M2 em todos os slots, a estimativa precisa
errar menos que ~0,1 ppm.

**Fonte 1 · integrador de portadora.** O receptor do DW1000 corrige a
diferença de frequência da portadora e expõe o valor em `DRX_CAR_INT`
(sub-registrador 0x27:28, 21 bits com sinal). Para 6,8 Mbps no canal 5:

$$
\hat\varepsilon_{car} = -\,C \cdot \frac{f_s}{2 \cdot 1024 \cdot 2^{17}} \cdot \frac{1}{f_c}
\approx -0{,}573\ \text{ppb} \times C
$$

com $f_s$ = 998,4 MHz e $f_c$ = 6489,6 MHz.

- Precisa ser lido junto com o timestamp de RX, antes de religar o RX: o
  próximo quadro sobrescreve o registrador.
- As constantes e o sinal precisam ser conferidos no *DW1000 User Manual*
  e validados em bancada no modo sombra (T11.10). Com o sinal trocado, o erro
  dobra em vez de cancelar.

**Fonte 2 · razão de timestamps entre rodadas.** Com dois POLLs da mesma
tag ($j < k$) recebidos pela mesma âncora:

$$
\hat\varepsilon_{ts} = \frac{t^{B}_{poll,rx}(k) - t^{B}_{poll,rx}(j)}{t^{A}_{poll,tx}(k) - t^{A}_{poll,tx}(j)} - 1
$$

- As diferenças são módulo 2⁴⁰ (`rtls_dw_ts_diff`). Aceitar só intervalos
  ≤ 1 s, longe da volta de 17,2 s.
- A base de 100 ms dilui o ruído do timestamp: ~1 ppb com 0,1 ns de jitter.
- O movimento da tag soma $v/c$: ~3,3 ppb a 1 m/s, ou ~2 mm no slot 3.
- Responde devagar a saltos de frequência, como os de ±0,5 ppm no
  aquecimento do cristal (APS011 §2.2).

**Política** (Kconfig `RTLS_TWR_SS_OFS_SRC_*`, proposta):

| Opção | Comportamento |
|-------|---------------|
| `CARRIER` | só a fonte 1 |
| `TIMESTAMPS` | só a fonte 2; a âncora fica sem distância até ter histórico |
| `BOTH` (padrão) | fonte 2 quando há histórico; fonte 1 na 1ª rodada, depois de uma lacuna > 1 s ou quando as duas divergem mais que um limiar (salto) |

- Limite de sanidade: \|ε̂\| acima de `RTLS_TWR_SS_OFS_MAX_PPB` descarta a
  medida e conta como inválida.
- Filtro exponencial opcional por âncora sobre ε̂, com peso
  `RTLS_TWR_SS_OFS_ALPHA_X100`. O valor sai da σ medida no T11.11.

**Referência de verdade para o desvio:** no modo sombra (rodando em DS), o
ToF do DS é praticamente exato. Ele dá o desvio que o SS precisaria:

$$
\varepsilon_{ref} = 1 - \frac{R_a - 2\,\widehat{ToF}_{DS}}{D_b}
$$

É ruidoso quadro a quadro (~0,3 ppm a 1 ms, pelo σ da distância), mas não
enviesado. A média de 1000 rodadas mede o viés de cada fonte com
~0,01 ppm.

**Implementação:** `double`, como no DS. As funções ficam em `twr_math`,
sem hardware: `rtls_twr_ss_tof()`, `rtls_twr_ss_ofs_from_carrier()` e
`rtls_twr_ss_ofs_from_ts()` (nomes provisórios).

## 3. Calibração do atraso de antena

O atraso de antena total de cada placa ($\delta = $ TX + RX) gera um viés
fixo. Com duas placas a uma distância conhecida $d_{real}$:

$$
b_{AB} = \frac{\bar d_{medida} - d_{real}}{c_{ar}\, t_u}\ \text{[ticks]},
\qquad b_{AB} = \frac{\Delta\delta_A + \Delta\delta_B}{2}
$$

Com **três placas** (A, B, C), medindo os três pares à mesma distância
conhecida:

$$
\Delta\delta_A = b_{AB} + b_{AC} - b_{BC}
$$

(e as permutações análogas para B e C). Soma-se $\Delta\delta_X$ ao atraso
total da placa X, dividindo igualmente entre TX e RX. O procedimento é o do
Decawave APS014:

1. Distância de **5,0 m** (zero do viés por potência no canal 5 / PRF 64,
   APS011 tab. 3), LOS, placas na mesma altura, antenas na mesma orientação.
   Anote a temperatura (2,15 mm/°C por placa, APS014).
2. Coletar pelo menos 1000 medidas por par.
3. Calcular $\Delta\delta$ e regravar os atrasos: Kconfig
   `CONFIG_RTLS_DW1000_{TX,RX}_ANT_DLY` até o M7, `settings` a partir do M8.
4. Validar a 1, 3 e 5 m (critério do M2).

### 3.1 Correção do viés dependente do RSL (APS011)

Mesmo calibrado, o timestamp do DW1000 varia com o nível do sinal recebido
(RSL). Para PRF de 64 MHz o efeito vai de −11 cm com sinal forte a +8 cm com
sinal fraco. Sem correção, só a 1 m (−10,5 cm) o critério de ±10 cm do M2
já estoura.

**Algoritmo** (aplicado na tag a cada distância DS-TWR):

1. Estimar o RSL pela equação de Friis em LOS, a partir da própria distância
   medida (canal 5, $c$ do vácuo, como no APS011):
   $P_R = -14{,}3 + G + 20\log_{10}(c) - 20\log_{10}(4\pi f_c d)$
   $\approx -62{,}99 + G - 20\log_{10}(d)$ dBm.
2. Interpolar linearmente a correção na tabela 2 do APS011 (PRF 64 MHz,
   −61 a −93 dBm em passos de 2 dB; fora da faixa, vale a extremidade).
3. $d_{corrigida} = d_{medida} - \text{correção}$.

| RSL (dBm) | −61 | −65 | −69 | −73 | **−77** | −81 | −85 | −89 | −93 |
|-----------|-----|-----|-----|-----|---------|-----|-----|-----|-----|
| Correção (cm) | −11,0 | −10,0 | −8,2 | −5,1 | **0,0** | +3,5 | +4,9 | +7,1 | +8,1 |

| Distância medida | RSL | Distância corrigida |
|------------------|-----|---------------------|
| 1,000 m | −63,0 dBm | 1,105 m |
| 5,000 m | −77,0 dBm | 5,000 m |
| 10,000 m | −83,0 dBm | 9,958 m |

**Por que Friis e não a potência medida pelo chip:** a estimativa do próprio
DW1000 perde exatidão justamente com sinal forte, onde o viés é maior. O
fabricante usa a distância medida.

**Configuração:**

| Símbolo Kconfig | Padrão | Efeito |
|-----------------|--------|--------|
| `CONFIG_RTLS_TWR_RANGE_BIAS_CORRECTION` | `y` | liga a correção |
| `CONFIG_RTLS_TWR_RANGE_BIAS_GAIN_DB_X10` | 0 | ganho das antenas menos perdas (dB × 10) |

> A tabela supõe o zero do viés em −77 dBm. Com G = 0, isso é **5,0 m**, a
> distância de calibração. Com G ≠ 0, a distância de calibração passa a ser
> 5,0 m × 10^(G/20). Como o ganho da antena da DWM1001 não foi caracterizado,
> G = 0 é uma aproximação.

**Limitações:** vale só em LOS (em NLOS o RSL real é menor que o previsto por
Friis), só para canal 5 / PRF 64, e o viés residual da própria correção
(~1 cm) não é modelado.

**Saída:** a tag guarda as duas distâncias (`distance_m` corrigida e
`distance_raw_m` bruta) e imprime as duas no CSV. A calibração usa a
corrigida, que a 5 m é praticamente igual à bruta.

## 4. Multilateração por mínimos quadrados (inicialização e baseline)

Dadas as âncoras $\mathbf a_i$ e as distâncias $d_i$, subtrai-se a equação da
âncora 0 de cada $\lVert \mathbf p - \mathbf a_i \rVert^2 = d_i^2$, o que dá
um sistema **linear**. O código resolve em $\mathbf q = \mathbf p - \mathbf a_0$,
para não perder precisão em `float` com coordenadas grandes:

$$
2(\mathbf a_i - \mathbf a_0)^\top \mathbf q = d_0^2 - d_i^2 + \lVert\mathbf a_i - \mathbf a_0\rVert^2,
\quad i = 1..N-1
\;\;\Rightarrow\;\; A\mathbf q = \mathbf b,
\quad \hat{\mathbf q} = (A^\top A)^{-1} A^\top \mathbf b
$$

As equações normais são acumuladas direto em $A^\top A$ (2×2 ou 3×3) e
resolvidas por Cholesky. Um pivô menor que $10^{-6}$ vezes a maior diagonal
indica geometria degenerada, e a função devolve `-EINVAL`.

- **3D:** exige ≥ 4 âncoras **não coplanares**.
- **2D com altura fixa** $z_t$: usa as distâncias horizontais
  $d_i' = \sqrt{d_i^2 - (z_t - z_i)^2}$. Exige ≥ 3 âncoras não colineares.
- **Refinamento Gauss-Newton** (até 3 iterações): com
  $J_i = (\hat{\mathbf p} - \mathbf a_i)^\top / \lVert \hat{\mathbf p} - \mathbf a_i \rVert$
  e $\Delta\mathbf p = (J^\top J)^{-1} J^\top (\mathbf d - h(\hat{\mathbf p}))$.
  Em 2D, $h_i$ usa a altura fixa: $h_i = \sqrt{(x - x_i)^2 + (y - y_i)^2 + (z_t - z_i)^2}$.
  Para antes se o passo ficar abaixo de 0,1 mm.
- **Qualidade da geometria:** $\text{GDOP} = \sqrt{\operatorname{tr}\big((J^\top J)^{-1}\big)}$.
  Valores altos indicam geometria ruim. `rtls_loc_multilaterate()` devolve o
  GDOP como diagnóstico.
- **Coerência da rodada:** a variância a posteriori
  $s^2 = \sum_i (d_i - h_i(\hat{\mathbf p}))^2 / (N - n)$, com $n$ = 2 ou 3,
  mede se as distâncias concordam entre si. Com ruído $\sigma_r$ e sem
  *outliers*, $s^2 \approx \sigma_r^2$.

## 5. Filtro de Kalman estendido (EKF)

### 5.1 Modelo

Estado de **velocidade constante**:

- 3D: $\mathbf x = [p_x, p_y, p_z, v_x, v_y, v_z]^\top$ (6 estados).
- 2D: $[p_x, p_y, v_x, v_y]^\top$ (4 estados), com $z = z_t$ fixo.

$$
\mathbf x_k = F\mathbf x_{k-1} + \mathbf w,\quad
F = \begin{bmatrix} I & \Delta t\, I \\ 0 & I \end{bmatrix},\quad
Q = \sigma_a^2 \begin{bmatrix} \tfrac{\Delta t^4}{4} I & \tfrac{\Delta t^3}{2} I \\ \tfrac{\Delta t^3}{2} I & \Delta t^2 I \end{bmatrix}
$$

O $\Delta t$ é o tempo **real** entre ciclos (timestamp do POLL), não os
100 ms nominais. $\sigma_a$ é `CONFIG_RTLS_LOC_ACCEL_STD_MMS2`.

### 5.2 Medida (uma por âncora)

$$
z_i = h_i(\mathbf x) + v_i,\quad
h_i(\mathbf x) = \lVert \mathbf p - \mathbf a_i \rVert,\quad
H_i = \left[\frac{(\mathbf p - \mathbf a_i)^\top}{\lVert \mathbf p - \mathbf a_i \rVert}\ \ \mathbf 0^\top\right],\quad
v_i \sim \mathcal N(0, \sigma_r^2)
$$

No modo 2D, $h_i$ inclui o termo fixo $(z_t - z_i)^2$ dentro da raiz.

### 5.3 Ciclo

1. **Predição:** $\hat{\mathbf x}^- = F\hat{\mathbf x}$, $P^- = FPF^\top + Q$.
2. **Atualização sequencial**, uma distância por vez, da maior para a menor
   potência recebida (`rx_power_cdbm`): o sinal mais forte tende a ser o mais
   confiável. Assim não há inversão de matriz, só escalares:
   - $\nu = z_i - h_i(\hat{\mathbf x})$, $S = H_i P H_i^\top + \sigma_r^2$
   - **Gating:** se $\nu^2/S > \gamma$ (χ² com 1 GL; $\gamma = 6{,}63$ para 99 %),
     a medida é rejeitada e o flag `OUTLIER` é marcado.
   - $K = P H_i^\top / S$, $\hat{\mathbf x} \leftarrow \hat{\mathbf x} + K\nu$
   - $P \leftarrow (I - K H_i) P (I - K H_i)^\top + K \sigma_r^2 K^\top$
     (forma de Joseph, numericamente estável em `float`)
3. **Simetrização:** $P \leftarrow (P + P^\top)/2$.

### 5.4 Inicialização e reinicialização

- Na primeira rodada com distâncias suficientes (2D: ≥ 3; 3D: ≥ 4), a
  posição vem da multilateração LS (§4), com velocidade nula. A saída leva o
  flag `INIT`. A covariância inicial é a do próprio LS, inflada se a rodada
  for incoerente:

$$
P_0 = \begin{bmatrix} \max(\sigma_r^2, s^2)\,(J^\top J)^{-1} & 0 \\ 0 & (1\,\text{m/s})^2 I \end{bmatrix}
$$

- Reinicializa (e já usa a rodada atual para a nova multilateração) quando:

| Condição | Motivo |
|----------|--------|
| 1 s sem nenhuma distância **aceita** | estado velho demais |
| > 5 rodadas seguidas com **metade ou mais** das distâncias rejeitadas, **e** a rodada atual coerente ($s^2 \le \gamma\,\sigma_r^2$) | o filtro está preso numa solução errada |
| soma das variâncias de posição > 100 m², ou estado não finito | proteção numérica |

> [!IMPORTANT]
> A regra original ("todas as medidas rejeitadas") não basta. Depois de um
> salto de ~3 m, o filtro pode ficar numa **solução espelhada**: a reflexão
> da posição real em torno da reta entre duas âncoras, onde essas duas
> distâncias batem e as outras duas são rejeitadas para sempre. A simulação
> do T4.13 mostrou isso em 3 de 5 sementes. Exigir metade ou mais rejeitadas
> resolve, e o teste de coerência evita reinicializar com uma rodada que tem,
> ela mesma, distâncias ruins (dois NLOS, por exemplo).

### 5.5 Sintonia inicial (ajustar no M4)

| Parâmetro | Valor inicial | Efeito |
|-----------|---------------|--------|
| $\sigma_r$ | 0,10 m | ↑ suaviza e confia menos nas medidas |
| $\sigma_a$ | 1,0 m/s² | ↑ responde mais rápido a manobras, com mais ruído |
| $\gamma$ | 6,63 | ↓ rejeita mais outliers, com risco de descartar medidas boas |

### 5.6 Custo computacional

Por distância, a forma de Joseph faz dois produtos $n \times n$ (2·$n^3$
multiplicações), mais a predição com outros dois. Em 2D ($n = 4$), com 4
distâncias, são ~750 multiplicações em `float` por ciclo; em 3D ($n = 6$),
~2 400. Na FPU do M4F a 64 MHz isso cabe com folga em 1 ms. A meta do M4 é
medida na tag com a API de *timing* do Zephyr: a linha `LOC:` do resumo de
10 s traz o tempo do último ciclo e o máximo (T4.9).

### 5.7 Testes em simulação

Resultados dos testes ztest rodando na DWM1001 (FPU ligada), com ruído
gaussiano de 5 cm, 4 âncoras no teto de uma sala de 8 × 6 m e tag a 1 m de
altura:

| ID | Caso | Meta | Obtido |
|----|------|------|--------|
| T4.1 | multilateração sem ruído, 2D e 3D | < 1 mm | ✅ |
| T4.3 | tag parada, 300 rodadas | RMSE ≤ 5 cm | 3,7 cm |
| T4.4 | reta a 1 m/s | RMSE ≤ 10 cm | 2,7 cm |
| T4.5 | +1 m numa âncora em 10 % das rodadas | rejeitadas, RMSE igual | 29/29 rejeitadas; 3,64 × 3,62 cm |
| T4.7 | NEES médio em 100 execuções | ≈ 4 | 4,18 |
| T4.13 | salto de 2,8 m | reinicia | reinicia em até 7 rodadas |

## 6. Saída e indicadores de qualidade

O `struct rtls_position` leva posição, velocidade e
$\sigma_{x,y,z} = \sqrt{\operatorname{diag}(P)}$. No ar, o quadro POSITION
leva posição e $\sigma$ em mm, mais `n_ranges` e os `flags`.

## 7. Métricas de avaliação

| Métrica | Definição |
|---------|-----------|
| Viés / σ de range | média / desvio padrão de $d_{medida} - d_{real}$ |
| RMSE de posição | $\sqrt{\frac1n\sum \lVert \hat{\mathbf p} - \mathbf p_{real}\rVert^2}$ |
| CEP50 / CEP95 | raio que contém 50 % / 95 % dos erros horizontais |
| Consistência | NEES médio: $\mathbf e^\top P^{-1} \mathbf e$ deve ficar ≈ dimensão do estado |
| Taxa efetiva | posições por segundo recebidas no host |
| Latência | instante do POLL → chegada no host |

## 8. Referências

- D. Neirynck, E. Luk, M. McLaughlin. *An alternative double-sided two-way
  ranging method*. WPNC 2016.
- Decawave/Qorvo. *DW1000 User Manual* (v2.18) e *DW1000 Datasheet*.
- Decawave/Qorvo. *APS011: Sources of error in DW1000 based TWR schemes*;
  *APS013: The implementation of two-way ranging with the DW1000*;
  *APS014: Antenna delay calibration of DW1000-based products and systems*.
- Y. Bar-Shalom, X. R. Li, T. Kirubarajan. *Estimation with Applications to
  Tracking and Navigation*. Wiley, 2001.
- D. Simon. *Optimal State Estimation*. Wiley, 2006.

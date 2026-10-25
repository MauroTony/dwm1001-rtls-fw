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
o nível de sinal recebido (Decawave APS011). No M2 se decide se vale aplicar
a tabela de correção em função de `rx_power`.

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
um sistema **linear**:

$$
2(\mathbf a_i - \mathbf a_0)^\top \mathbf p = d_0^2 - d_i^2 + \lVert\mathbf a_i\rVert^2 - \lVert\mathbf a_0\rVert^2,
\quad i = 1..N-1
\;\;\Rightarrow\;\; A\mathbf p = \mathbf b,
\quad \hat{\mathbf p} = (A^\top A)^{-1} A^\top \mathbf b
$$

- **3D:** exige ≥ 4 âncoras **não coplanares**.
- **2D com altura fixa** $z_t$: usa as distâncias horizontais
  $d_i' = \sqrt{d_i^2 - (z_t - z_i)^2}$. Exige ≥ 3 âncoras não colineares.
- **Refinamento Gauss-Newton** (2–3 iterações): com
  $J_i = (\hat{\mathbf p} - \mathbf a_i)^\top / \lVert \hat{\mathbf p} - \mathbf a_i \rVert$
  e $\Delta\mathbf p = (J^\top J)^{-1} J^\top (\mathbf d - h(\hat{\mathbf p}))$.
- **Qualidade da geometria:** $\text{GDOP} = \sqrt{\operatorname{tr}\big((J^\top J)^{-1}\big)}$.
  Valores altos indicam geometria ruim. Fica registrado como diagnóstico.

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
2. **Atualização sequencial**, uma distância por vez, em ordem crescente de
   $\sigma$ ou de potência. Assim não há inversão de matriz, só escalares:
   - $\nu = z_i - h_i(\hat{\mathbf x})$, $S = H_i P H_i^\top + \sigma_r^2$
   - **Gating:** se $\nu^2/S > \gamma$ (χ² com 1 GL; $\gamma = 6{,}63$ para 99 %),
     a medida é rejeitada e o flag `OUTLIER` é marcado.
   - $K = P H_i^\top / S$, $\hat{\mathbf x} \leftarrow \hat{\mathbf x} + K\nu$
   - $P \leftarrow (I - K H_i) P (I - K H_i)^\top + K \sigma_r^2 K^\top$
     (forma de Joseph, numericamente estável em `float`)
3. **Simetrização:** $P \leftarrow (P + P^\top)/2$.

### 5.4 Inicialização e reinicialização

- Na primeira rodada com distâncias suficientes, a posição vem da
  multilateração LS (§4), com velocidade nula e
  $P_0 = \operatorname{diag}(\sigma_{LS}^2, \ldots, (1\,\text{m/s})^2)$.
- Reinicializa quando: 1 s sem atualização, mais de 5 rodadas seguidas com
  todas as medidas rejeitadas, ou traço de P acima de um limite.

### 5.5 Sintonia inicial (ajustar no M4)

| Parâmetro | Valor inicial | Efeito |
|-----------|---------------|--------|
| $\sigma_r$ | 0,10 m | ↑ suaviza e confia menos nas medidas |
| $\sigma_a$ | 1,0 m/s² | ↑ responde mais rápido a manobras, com mais ruído |
| $\gamma$ | 6,63 | ↓ rejeita mais outliers, com risco de descartar medidas boas |

### 5.6 Custo computacional

Em 3D, com 6 estados e 4 medidas, são algumas centenas de operações em
`float` por ciclo. Na FPU do M4F a 64 MHz isso fica bem abaixo de 1 ms (meta
do M4, medida com `k_cycle_get_32`).

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

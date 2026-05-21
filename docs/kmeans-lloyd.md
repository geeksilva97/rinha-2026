# K-Means (Lloyd's algorithm)

**Entrada:**
- Dataset $X = \{x_1, x_2, \ldots, x_N\}$, com $x_i \in \mathbb{R}^D$
- Número de clusters $K$
- Máximo de iterações $T$

**Saída:** Centróides $C = \{c_1, \ldots, c_K\}$ e atribuições $a: \{1,\ldots,N\} \to \{1,\ldots,K\}$

## Passo 1 — Inicialização (Forgy)

Escolha $K$ índices aleatórios distintos $I = \{i_1, \ldots, i_K\} \subset \{1,\ldots,N\}$.

$$c_j = x_{i_j}, \quad j = 1,\ldots,K$$

## Passo 2 — Para $t = 1, 2, \ldots, T$

**2a) Assignment** — atribui cada ponto ao centróide mais próximo:

$$a(i) = \arg\min_{j \in \{1,\ldots,K\}} \|x_i - c_j\|^2, \quad i = 1,\ldots,N$$

onde

$$\|x_i - c_j\|^2 = \sum_{d=1}^{D}(x_{i,d} - c_{j,d})^2$$

**2b) Update** — recalcula cada centróide como média dos pontos atribuídos:

$$S_j = \{i : a(i) = j\}$$

$$c_j = \frac{1}{|S_j|} \sum_{i \in S_j} x_i, \quad \text{se } |S_j| > 0$$

## Passo 3 — Critério de parada

Para se qualquer um:
- Nenhuma atribuição mudou: $a^{(t)}(i) = a^{(t-1)}(i),\ \forall i$
- Movimento dos centróides abaixo do limite: $\sum_{j} \|c_j^{(t)} - c_j^{(t-1)}\| < \varepsilon$
- $t = T$

## Notas

- Distância ao quadrado basta — comparar $\|a\|^2 < \|b\|^2$ é equivalente a $\|a\| < \|b\|$ (mais rápido, sem `sqrt`).
- Se $|S_j| = 0$ (centróide órfão): mantenha o anterior ou re-inicialize com ponto aleatório.
- Lloyd converge garantidamente, mas pra um mínimo local — não necessariamente global.

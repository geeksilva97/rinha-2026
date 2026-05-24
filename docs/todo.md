# TODO

## Otimizações pendentes do k-means

- **SIMD na distância L2**: o loop interno (linha ~50 de `native/k_means.cpp`) percorre 14 dims somando diff². 14 floats cabem em 2 registradores AVX-256 (8 floats cada) ou 4 SSE (4 floats). Ganho esperado: 4-8×.
- **Paralelismo no ASSIGN**: cada ponto é independente dos outros — `#pragma omp parallel for` na linha do `for (i = 0; ...)` deve escalar quase linear no número de cores.
- **Reduzir alocações**: `sum` e `count` já estão fora do loop. Pode reusar `assignments` antigo como buffer pra detectar mudanças (early exit por convergência).
- **Medir build time** com k=1700, N=3M: atualmente ~3:23 single-threaded. Meta: < 30s pra iteração rápida no Docker build.

## Runtime (Ruby)

- Ler `ivf.bin` mmap-eado entre os dois processos da API (economiza 168 MB).
- Decodificar o payload da rinha em `to_vec` (14 dims normalizadas).
- Busca: distância pros K=1700 centróides → escolher `nprobe` mais próximos → varrer inverted lists → top-5 → fraud_score.

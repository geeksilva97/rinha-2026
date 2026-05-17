# rinha-2026 — Fraud Detection

Minha submissão pra [Rinha de Backend 2026](https://github.com/zanfranceschi/rinha-de-backend-2026). O tema da edição é **detecção de fraudes via busca vetorial**, com os limites de sempre: pouca RAM, pouca CPU, e a obrigação de não fazer besteira.

## Stack

- **Ruby 4.0.2** + Rack 3.2 + Puma 8.0 + Oj 3.17
- **SQLite** com a extensão [`vec1`](https://sqlite.org/vec1/) (a oficial do time do SQLite, não a `sqlite-vec` do GitHub)
- **kNN aproximado** sobre IVFADC + product quantization
- **jemalloc** via `LD_PRELOAD` no container

## Limites

| Recurso | Total | Por instância (LB + 2 apps) |
|---|---|---|
| RAM    | 350 MB | ~120–150 MB |
| CPU    | 1.0    | ~0.45 |

Esses limites mandam em tudo. Cada decisão de arquitetura volta pra essa tabela.

## Abordagem

A ideia toda é: pega o payload da transação, converte num vetor de features (`lib/to_vec.rb`), consulta os `k` vizinhos mais próximos num dataset rotulado, e devolve a fração de vizinhos marcados como fraude como score.

```
POST /fraud-score → to_vec(payload) → vec1 kNN(k=25) → AVG(is_fraud) → resposta
```

### Por que vec1 + ANN treinado em vez de força bruta

Não é por velocidade — com 14 dimensões a força bruta seria rápida. **É compressão.** Vetores f32 crus custam 56 bytes por linha; com `codesize:8` em product quantization isso vira 8 bytes. Num dataset de ~1–2M linhas, é a diferença entre ~100 MB e ~15 MB no page cache. Só a versão quantizada cabe no orçamento de RAM.

O treino acontece **uma vez, offline, na máquina hospedeira** — não dentro do container. O container só recebe o `fraud.db` pronto.

### Pipeline

```
host (8 GB de RAM, sem limites):
  resources/references.json.gz  (~50 MB compactado, ~285 MB cru)
       │
       ▼
  bin/ingest.rb  → INSERTs em fraud.db
       │
       ▼
  bin/train.rb   → vec1_train(...) → INSERT ('rebuild', :model)
       │
       ▼
  fraud.db (final, indexado, quantizado)

docker build:
  COPY fraud.db /app/
  COPY vec1.so  /app/

container (350 MB / 1 CPU divididos):
  abre fraud.db read-only, consulta. Nunca treina, nunca faz ingest.
```

Ambas as instâncias do Puma abrem o mesmo `fraud.db` em modo read-only. O page cache do SO é compartilhado entre processos — o dataset é pago em RAM **uma vez**, não duplicado por instância.

## Puma

Sem cluster mode. Dois workers significariam dois heaps de Ruby, e isso estoura o orçamento.

```ruby
workers 0          # processo único — sem fork, sem heap duplicado
threads 1, 4       # pool pequeno; sqlite3 libera o GVL nas queries
bind "tcp://0.0.0.0:9999"
```

Threads compensam porque o gem `sqlite3` libera o GVL durante as chamadas pra SQLite. Mais que 4 só adiciona contenção sem retorno.

## jemalloc

Glibc malloc no Linux fragmenta feio sob threads e não devolve memória pro SO. jemalloc corta uns 20–40% do RSS estável sem mudar uma linha de código. Sob orçamento apertado, não é opcional:

```dockerfile
RUN apt-get update && apt-get install -y libjemalloc2 && rm -rf /var/lib/apt/lists/*
ENV LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2
ENV MALLOC_CONF="narenas:2,background_thread:true,dirty_decay_ms:1000,muzzy_decay_ms:0"
```

`narenas:2` mantém o número de arenas baixo (poucas threads, pouca fragmentação). `dirty_decay_ms:1000` devolve páginas livres rápido em vez de segurar (o default de 10s mantém o RSS alto sem motivo).

## Estrutura do repo

```
.
├── config.ru             # endpoint /fraud-score
├── lib/
│   └── to_vec.rb         # payload → vetor de features
├── bin/
│   ├── ingest.rb         # JSON → fraud.db (offline)
│   └── train.rb          # treino do índice ANN (offline)
├── vendor/
│   └── vec1/             # extensão vec1 (vendored)
├── resources/            # dataset gzipado, payloads de exemplo, normalização
├── puma.rb
├── Dockerfile
└── CLAUDE.md             # decisões de arquitetura + raciocínio
```

## Build local

```bash
# Extensão vec1 (macOS arm64, dev)
cd vendor/vec1
cc -O3 -DNDEBUG vec1.c -shared -fPIC -Wl,-undefined,dynamic_lookup -o vec1.dylib

# (No container Linux x86_64, o Dockerfile compila com -mavx2 -mfma)
```

## Referências

- [Repo oficial do desafio](https://github.com/zanfranceschi/rinha-de-backend-2026)
- [`vec1`](https://sqlite.org/vec1/) — extensão oficial de busca vetorial pro SQLite
- [`CLAUDE.md`](./CLAUDE.md) — notas mais densas sobre as decisões, pra quando precisar lembrar por que algo foi feito

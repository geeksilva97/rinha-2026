require 'oj'

# Converte payload da rinha em vetor 14-dim normalizado.
# Schema documentado em docs.rinha REGRAS_DE_DETECCAO.md.
module Vectorizer
  RESOURCES = File.expand_path('resources', __dir__)

  MCC_RISK = Oj.load(File.read(File.join(RESOURCES, 'mcc_risk.json'))).freeze

  _norm = Oj.load(File.read(File.join(RESOURCES, 'normalization.json')))
  MAX_AMOUNT              = _norm.fetch('max_amount').to_f
  MAX_INSTALLMENTS        = _norm.fetch('max_installments').to_f
  AMOUNT_VS_AVG_RATIO     = _norm.fetch('amount_vs_avg_ratio').to_f
  MAX_MINUTES             = _norm.fetch('max_minutes').to_f
  MAX_KM                  = _norm.fetch('max_km').to_f
  MAX_TX_COUNT_24H        = _norm.fetch('max_tx_count_24h').to_f
  MAX_MERCHANT_AVG_AMOUNT = _norm.fetch('max_merchant_avg_amount').to_f

  # Sakamoto: tabela de offset por mês (jan..dez). 0=Domingo,...,6=Sábado.
  DOW_TABLE = [0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4].freeze

  # Parseia ISO "2026-03-11T18:45:53Z" via byte arithmetic. Zero alocação.
  # Retorna [year, month, day, hour, minute, second].
  def self.parse_iso(s)
    [
      (s.getbyte(0) - 48) * 1000 + (s.getbyte(1) - 48) * 100 + (s.getbyte(2) - 48) * 10 + (s.getbyte(3) - 48),
      (s.getbyte(5) - 48) * 10 + (s.getbyte(6) - 48),
      (s.getbyte(8) - 48) * 10 + (s.getbyte(9) - 48),
      (s.getbyte(11) - 48) * 10 + (s.getbyte(12) - 48),
      (s.getbyte(14) - 48) * 10 + (s.getbyte(15) - 48),
      (s.getbyte(17) - 48) * 10 + (s.getbyte(18) - 48)
    ]
  end

  # Converte payload em vetor 14-dim normalizado.
  # Aceita `out` opcional pra reuso de buffer (thread-local).
  def self.to_vec(payload, out = Array.new(14, 0.0))
    tx       = payload['transaction']
    customer = payload['customer']
    merchant = payload['merchant']
    terminal = payload['terminal']
    last_tx  = payload['last_transaction']

    iso = tx['requested_at']
    year   = (iso.getbyte(0) - 48) * 1000 + (iso.getbyte(1) - 48) * 100 + (iso.getbyte(2) - 48) * 10 + (iso.getbyte(3) - 48)
    month  = (iso.getbyte(5) - 48) * 10 + (iso.getbyte(6) - 48)
    day    = (iso.getbyte(8) - 48) * 10 + (iso.getbyte(9) - 48)
    hour   = (iso.getbyte(11) - 48) * 10 + (iso.getbyte(12) - 48)

    # Sakamoto: 0=Dom..6=Sáb. Spec quer 0=Seg..6=Dom → desloca em -1 mod 7.
    y_adj   = month < 3 ? year - 1 : year
    sak_dow = (y_adj + y_adj / 4 - y_adj / 100 + y_adj / 400 + DOW_TABLE[month - 1] + day) % 7
    dow     = (sak_dow + 6) % 7

    amount       = tx['amount'].to_f
    installments = tx['installments']
    customer_avg = customer['avg_amount'].to_f

    # 0: amount / max_amount, clamp [0,1]
    v = amount / MAX_AMOUNT
    out[0] = v < 0 ? 0.0 : (v > 1 ? 1.0 : v)

    # 1: installments / max_installments, clamp [0,1]
    v = installments / MAX_INSTALLMENTS
    out[1] = v > 1 ? 1.0 : v

    # 2: (amount/avg) / ratio, clamp [0,1]
    v = (amount / customer_avg) / AMOUNT_VS_AVG_RATIO
    out[2] = v > 1 ? 1.0 : v

    # 3: hora / 23
    out[3] = hour / 23.0

    # 4: dia_semana / 6
    out[4] = dow / 6.0

    # 5,6: depend de last_transaction
    if last_tx.nil?
      out[5] = -1.0
      out[6] = -1.0
    else
      l    = last_tx['timestamp']
      lyr  = (l.getbyte(0) - 48) * 1000 + (l.getbyte(1) - 48) * 100 + (l.getbyte(2) - 48) * 10 + (l.getbyte(3) - 48)
      lmo  = (l.getbyte(5) - 48) * 10 + (l.getbyte(6) - 48)
      lda  = (l.getbyte(8) - 48) * 10 + (l.getbyte(9) - 48)
      lhr  = (l.getbyte(11) - 48) * 10 + (l.getbyte(12) - 48)
      lmi  = (l.getbyte(14) - 48) * 10 + (l.getbyte(15) - 48)
      lse  = (l.getbyte(17) - 48) * 10 + (l.getbyte(18) - 48)

      cmi = (iso.getbyte(14) - 48) * 10 + (iso.getbyte(15) - 48)
      cse = (iso.getbyte(17) - 48) * 10 + (iso.getbyte(18) - 48)

      # Delta em minutos via Time.utc.to_i (2 alocações; lida com mês/ano)
      delta_min = (Time.utc(year, month, day, hour, cmi, cse).to_i -
                   Time.utc(lyr, lmo, lda, lhr, lmi, lse).to_i) / 60.0

      v = delta_min / MAX_MINUTES
      out[5] = v < 0 ? 0.0 : (v > 1 ? 1.0 : v)

      v = last_tx['km_from_current'].to_f / MAX_KM
      out[6] = v < 0 ? 0.0 : (v > 1 ? 1.0 : v)
    end

    # 7: km_from_home / max_km, clamp [0,1]
    v = terminal['km_from_home'].to_f / MAX_KM
    out[7] = v < 0 ? 0.0 : (v > 1 ? 1.0 : v)

    # 8: tx_count_24h / max_tx_count_24h, clamp [0,1]
    v = customer['tx_count_24h'] / MAX_TX_COUNT_24H
    out[8] = v > 1 ? 1.0 : v

    # 9: is_online
    out[9] = terminal['is_online'] ? 1.0 : 0.0

    # 10: card_present
    out[10] = terminal['card_present'] ? 1.0 : 0.0

    # 11: unknown_merchant (1 se desconhecido, 0 se conhecido)
    out[11] = customer['known_merchants'].include?(merchant['id']) ? 0.0 : 1.0

    # 12: mcc_risk lookup (default 0.5)
    out[12] = MCC_RISK[merchant['mcc']] || 0.5

    # 13: merchant.avg_amount / max_merchant_avg_amount, clamp [0,1]
    v = merchant['avg_amount'].to_f / MAX_MERCHANT_AVG_AMOUNT
    out[13] = v < 0 ? 0.0 : (v > 1 ? 1.0 : v)

    out
  end
end

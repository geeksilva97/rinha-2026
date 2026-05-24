# Bench: ternário inline vs Float#clamp na conversão de payload em vetor 14-dim.
# Usa os payloads de exemplo da rinha como entrada realista.
#
# Roda com:  ruby -I.. bench/clamp_vs_ternary.rb        (sem YJIT)
#            ruby --yjit -I.. bench/clamp_vs_ternary.rb (com YJIT)

require 'benchmark/ips'
require 'oj'
require_relative '../vectorizer'

PAYLOADS = Oj.load(File.read(File.expand_path('../resources/example-payloads.json', __dir__)))

# Versão com Float#clamp (substituto do ternário)
module VectorizerClamp
  MCC_RISK                = Vectorizer::MCC_RISK
  MAX_AMOUNT              = Vectorizer::MAX_AMOUNT
  MAX_INSTALLMENTS        = Vectorizer::MAX_INSTALLMENTS
  AMOUNT_VS_AVG_RATIO     = Vectorizer::AMOUNT_VS_AVG_RATIO
  MAX_MINUTES             = Vectorizer::MAX_MINUTES
  MAX_KM                  = Vectorizer::MAX_KM
  MAX_TX_COUNT_24H        = Vectorizer::MAX_TX_COUNT_24H
  MAX_MERCHANT_AVG_AMOUNT = Vectorizer::MAX_MERCHANT_AVG_AMOUNT
  DOW_TABLE               = Vectorizer::DOW_TABLE

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

    y_adj   = month < 3 ? year - 1 : year
    sak_dow = (y_adj + y_adj / 4 - y_adj / 100 + y_adj / 400 + DOW_TABLE[month - 1] + day) % 7
    dow     = (sak_dow + 6) % 7

    amount       = tx['amount'].to_f
    installments = tx['installments']
    customer_avg = customer['avg_amount'].to_f

    out[0] = (amount / MAX_AMOUNT).clamp(0.0, 1.0)
    out[1] = (installments / MAX_INSTALLMENTS).clamp(0.0, 1.0)
    out[2] = ((amount / customer_avg) / AMOUNT_VS_AVG_RATIO).clamp(0.0, 1.0)
    out[3] = hour / 23.0
    out[4] = dow / 6.0

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
      cmi  = (iso.getbyte(14) - 48) * 10 + (iso.getbyte(15) - 48)
      cse  = (iso.getbyte(17) - 48) * 10 + (iso.getbyte(18) - 48)

      delta_min = (Time.utc(year, month, day, hour, cmi, cse).to_i -
                   Time.utc(lyr, lmo, lda, lhr, lmi, lse).to_i) / 60.0

      out[5] = (delta_min / MAX_MINUTES).clamp(0.0, 1.0)
      out[6] = (last_tx['km_from_current'].to_f / MAX_KM).clamp(0.0, 1.0)
    end

    out[7]  = (terminal['km_from_home'].to_f / MAX_KM).clamp(0.0, 1.0)
    out[8]  = (customer['tx_count_24h'] / MAX_TX_COUNT_24H).clamp(0.0, 1.0)
    out[9]  = terminal['is_online'] ? 1.0 : 0.0
    out[10] = terminal['card_present'] ? 1.0 : 0.0
    out[11] = customer['known_merchants'].include?(merchant['id']) ? 0.0 : 1.0
    out[12] = MCC_RISK[merchant['mcc']] || 0.5
    out[13] = (merchant['avg_amount'].to_f / MAX_MERCHANT_AVG_AMOUNT).clamp(0.0, 1.0)

    out
  end
end

# Sanity: as duas versões devem produzir o mesmo vetor pra qualquer payload
PAYLOADS.each_with_index do |p, i|
  a = Vectorizer.to_vec(p)
  b = VectorizerClamp.to_vec(p)
  unless a == b
    warn "MISMATCH no payload #{i}:"
    warn "  ternary: #{a.inspect}"
    warn "  clamp:   #{b.inspect}"
    exit 1
  end
end
puts "✓ saídas idênticas pros #{PAYLOADS.size} payloads"
puts "ruby: #{RUBY_VERSION}  yjit: #{defined?(RubyVM::YJIT) && RubyVM::YJIT.enabled? ? "ON" : "off"}"
puts

# Buffers reusáveis pra evitar alocação durante o benchmark
buf_t = Array.new(14, 0.0)
buf_c = Array.new(14, 0.0)

# 1) Single payload (caso típico de uma única request)
p0 = PAYLOADS[0]
Benchmark.ips do |x|
  x.config(time: 5, warmup: 2)
  x.report('ternario (1 payload)') { Vectorizer.to_vec(p0, buf_t) }
  x.report('clamp    (1 payload)') { VectorizerClamp.to_vec(p0, buf_c) }
  x.compare!
end

# 2) Loop nos N payloads (mais robusto contra branch prediction tendenciosa)
Benchmark.ips do |x|
  x.config(time: 5, warmup: 2)
  x.report('ternario (todos)') { PAYLOADS.each { |p| Vectorizer.to_vec(p, buf_t) } }
  x.report('clamp    (todos)') { PAYLOADS.each { |p| VectorizerClamp.to_vec(p, buf_c) } }
  x.compare!
end

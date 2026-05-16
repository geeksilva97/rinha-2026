# frozen_string_literal: true

PARAMS = {
  max_amount: 10_000,
  max_installments: 12,
  amount_vs_avg_ratio: 10,
  max_minutes: 1440,
  max_km: 1000,
  max_tx_count_24h: 20,
  max_merchant_avg_amount: 10_000
}.freeze

def day_of_week(y, m, d)
  if m < 3
    m += 12
    y -= 1
  end
  k = y % 100
  j = y / 100
  h = (d + 13 * (m + 1) / 5 + k + k/4 + j/4 + 5*j) % 7
  # h: 0=Saturday, 1=Sunday, ..., 6=Friday
  (h + 5) % 7  # convert to 0=Monday ... 6=Sunday if you prefer
end

def to_vec(payload, mcc_risk={})
  pp payload
  s = payload['transaction']['requested_at'] 
  year   = s[0, 4].to_i
  month  = s[5, 2].to_i
  day    = s[8, 2].to_i

  last_txn = payload['last_transaction']
  minutes_since_last_tx = -1
  km_from_last_tx = -1

  unless last_txn.nil?
    minutes_since_last_tx = last_txn['timestamp'][14, 2].to_f / PARAMS[:max_minutes]
    km_from_last_tx = last_txn['km_from_current'].to_f / PARAMS[:max_km]
  end

  [
    (payload['transaction']['amount'].to_f / PARAMS[:max_amount]).clamp(0.0, 1.0),
    (payload['transaction']['installments'].to_f / PARAMS[:max_installments]).clamp(0.0, 1.0),
    (payload['transaction']['amount'] / payload['customer']['avg_amount'] / PARAMS[:amount_vs_avg_ratio]).clamp(0.0, 1.0),
    payload['transaction']['requested_at'][8, 2].to_f / 23, # hour_of_day
    day_of_week(year, month, day).to_f / 6, # day_of_week
    minutes_since_last_tx,
    km_from_last_tx,
    (payload['terminal']['km_from_home'].to_f / PARAMS[:max_km]).clamp(0.0, 1.0),
    (payload['customer']['tex_count_24h'].to_f / PARAMS[:max_tx_count_24h]).clamp(0.0, 1.0),
    (payload['terminal']['is_online'] ? 1.0 : 0.0),
    (payload['terminal']['card_present'] ? 1.0 : 0.0),
    (payload['customer']['known_merchants'].include? payload['merchant']['id'])? 0.0 : 1.0,
    mcc_risk[payload['merchant']['id']],
    (payload['merchant']['avg_amount'].to_f / PARAMS[:max_merchant_avg_amount]).clamp(0.0, 1.0)
  ]
end

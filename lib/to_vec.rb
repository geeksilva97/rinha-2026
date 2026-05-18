# frozen_string_literal: true

def day_of_week(y, m, d)
  if m < 3
    m += 12
    y -= 1
  end
  k = y % 100
  j = y / 100
  h = (d + 13 * (m + 1) / 5 + k + k/4 + j/4 + 5*j) % 7
  # h: 0=Saturday, 1=Sunday, ..., 6=Friday
  (h + 5) % 7  # 0=Monday ... 6=Sunday
end

# Parse "YYYY-MM-DDTHH:MM:SSZ" into a UTC Time. Avoids Time.iso8601 / Time.parse
# overhead (regex, validation) since the format is guaranteed by the spec.
def parse_iso_utc(s)
  Time.utc(s[0, 4].to_i, s[5, 2].to_i, s[8, 2].to_i, s[11, 2].to_i, s[14, 2].to_i, s[17, 2].to_i)
end

def to_vec(payload, mcc_risk, params)
  s     = payload['transaction']['requested_at']
  year  = s[0, 4].to_i
  month = s[5, 2].to_i
  day   = s[8, 2].to_i
  hour  = s[11, 2].to_i

  last_txn = payload['last_transaction']
  minutes_since_last_tx = -1.0
  km_from_last_tx       = -1.0

  unless last_txn.nil?
    delta_minutes = (parse_iso_utc(s) - parse_iso_utc(last_txn['timestamp'])) / 60.0
    minutes_since_last_tx = (delta_minutes / params['max_minutes']).clamp(0.0, 1.0)
    km_from_last_tx       = (last_txn['km_from_current'].to_f / params['max_km']).clamp(0.0, 1.0)
  end

  [
    (payload['transaction']['amount'].to_f / params['max_amount']).clamp(0.0, 1.0),
    (payload['transaction']['installments'].to_f / params['max_installments']).clamp(0.0, 1.0),
    (payload['transaction']['amount'] / payload['customer']['avg_amount'] / params['amount_vs_avg_ratio']).clamp(0.0, 1.0),
    hour.to_f / 23,
    day_of_week(year, month, day).to_f / 6,
    minutes_since_last_tx,
    km_from_last_tx,
    (payload['terminal']['km_from_home'].to_f / params['max_km']).clamp(0.0, 1.0),
    (payload['customer']['tx_count_24h'].to_f / params['max_tx_count_24h']).clamp(0.0, 1.0),
    payload['terminal']['is_online']    ? 1.0 : 0.0,
    payload['terminal']['card_present'] ? 1.0 : 0.0,
    payload['customer']['known_merchants'].include?(payload['merchant']['id']) ? 0.0 : 1.0,
    mcc_risk.fetch(payload['merchant']['mcc'], 0.5),
    (payload['merchant']['avg_amount'].to_f / params['max_merchant_avg_amount']).clamp(0.0, 1.0)
  ]
end

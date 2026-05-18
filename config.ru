require 'oj'
require 'sqlite3'
require_relative 'lib/to_vec'

Oj.default_options = { mode: :strict }

MCC_RISK      = Oj.load(File.read('resources/mcc_risk.json')).freeze
NORMALIZATION = Oj.load(File.read('resources/normalization.json')).freeze

VEC1_EXT  = ENV.fetch('VEC1_EXT', './vendor/vec1/vec1.dylib')
NPROBE    = ENV.fetch('NPROBE', '8').to_i
KNN_QUERY = "SELECT is_fraud FROM vectors(?, '{K:5, nprobe:#{NPROBE}}')"
JSON_HDR  = { 'content-type' => 'application/json' }.freeze

def stmt
  Thread.current[:stmt] ||= begin
    db = SQLite3::Database.new('fraud.db', readonly: true)
    db.enable_load_extension(true)
    db.load_extension(VEC1_EXT)
    db.enable_load_extension(false)
    db.prepare(KNN_QUERY)
  end
end

run do |env|
  case [env['REQUEST_METHOD'], env['PATH_INFO']]
  when ['GET', '/ready']
    [200, {}, ['ok']]

  when ['POST', '/fraud-score']
    payload = Oj.load(env['rack.input'].read)
    vec     = to_vec(payload, MCC_RISK, NORMALIZATION)
    blob    = SQLite3::Blob.new(vec.pack('e*'))
    rows    = stmt.execute(blob).to_a
    score   = rows.count { |r| r[0] == 1 } / 5.0
    body    = Oj.dump({ approved: score < 0.6, fraud_score: score }, mode: :compat)
    [200, JSON_HDR, [body]]
  else
    [404, {}, ['not found']]
  end
end

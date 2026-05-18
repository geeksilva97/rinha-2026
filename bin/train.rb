# frozen_string_literal: true

require 'sqlite3'
require 'json'

DB_PATH      = 'fraud.db'
EXTENSION    = './vendor/vec1/vec1.dylib'
SAMPLE_SIZE  = 500_000
CONFIG = {
  distance: 'l2',
  nbucket:  1024,
  codesize: 8,
  opq:      true,
  # nthread: 1 — multi-threaded encoding has a race that corrupts ~1 row in 3M
  # ("integrity_check: PQ does not match calculated PQ for row N"). Single-
  # threaded takes ~3x longer (~90s vs ~30s for this dataset) but is correct.
  nthread:  1,
  progress: 'progress'
}.freeze

db = SQLite3::Database.new(DB_PATH)
db.enable_load_extension(true)
db.load_extension(EXTENSION)

db.execute 'PRAGMA journal_mode = OFF'
db.execute 'PRAGMA synchronous  = OFF'
db.execute 'PRAGMA temp_store   = MEMORY'

db.create_function('progress', 2) do |fn, percent, msg|
  fn.result = nil
  puts "[train] #{percent.to_s.rjust(3)}%  #{msg}"
end

before = File.size(DB_PATH)
puts "before: #{(before / 1024.0 / 1024.0).round(1)} MB"

t = Time.now
db.execute_batch <<~SQL
  INSERT INTO vectors(cmd, vector) VALUES('rebuild', (
    SELECT vec1_train(vector, '#{CONFIG.to_json}')
    FROM vectors WHERE rowid IN (
      SELECT rowid FROM vectors ORDER BY RANDOM() LIMIT #{SAMPLE_SIZE}
    )
  ));
SQL
puts "train+rebuild: #{(Time.now - t).round(1)}s"

after = File.size(DB_PATH)
puts "after:  #{(after / 1024.0 / 1024.0).round(1)} MB  (#{(100.0 * after / before).round(1)}% of original)"

ok = db.execute('PRAGMA integrity_check').first.first
raise "integrity_check failed: #{ok}" unless ok == 'ok'
puts "integrity_check: ok"

t = Time.now
db.execute('VACUUM')
puts "vacuum: #{(Time.now - t).round(1)}s, final size: #{(File.size(DB_PATH) / 1024.0 / 1024.0).round(1)} MB"

q = [0.5] * 14
rows = db.execute(
  "SELECT rowid, is_fraud FROM vectors(?, '{K:10, nprobe:8}')",
  [SQLite3::Blob.new(q.pack('e*'))]
)
puts "sanity query (k=10): #{rows.size} rows back"
pp rows

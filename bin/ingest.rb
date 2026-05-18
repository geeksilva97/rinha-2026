# frozen_string_literal: true

require 'oj'
require 'sqlite3'
require 'zlib'

class MySaj < Oj::Saj
  def initialize
    super
    @batch = []
    @vec = []
    @label = ''
    @db = SQLite3::Database.new 'fraud.db'
    init_db
  end

  def init_db
    @db.execute 'PRAGMA journal_mode = OFF'
    @db.execute 'PRAGMA synchronous  = OFF'
    @db.execute 'PRAGMA temp_store   = MEMORY'

    @db.enable_load_extension(true)
    @db.load_extension('./vendor/vec1/vec1.dylib')
    @db.execute <<-SQL
     CREATE VIRTUAL TABLE IF NOT EXISTS vectors USING vec1(vector, is_fraud)
    SQL
  end

  def array_start(key)
    @vec = [] if key == 'vector'
  end

  def add_value(value, key)
    if key.nil?
      @vec << value.to_f
    else
      @label = value == 'fraud' ? 1 : 0
    end
  end

  def hash_end(_key)
    # No mid-stream flushing: accumulating the whole ~3M-row dataset peaks
    # at ~800MB RSS on the host (8GB), and a single transaction with
    # synchronous=OFF runs in ~10s. Splitting into batches is slower.
    @batch << [@vec, @label]
  end

  def flush
    @db.transaction do
      puts "[#{Process.pid}] inserting #{@batch.size} items"

      @batch.each do |vec, label|
        @db.execute('INSERT INTO vectors VALUES (?, ?)', [
          SQLite3::Blob.new(vec.pack('e*')),
          label
        ])
      end
    end

    @batch = []
  end
end

handler = MySaj.new
parser = Oj::Parser.new(:saj)
parser.handler = handler
Zlib::GzipReader.open('resources/references.json.gz') do |gz|
  parser.load(gz)
  handler.flush
end

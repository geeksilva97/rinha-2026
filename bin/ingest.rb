# frozen_string_literal: true

require 'oj'
require 'sqlite3'
require 'zlib'

class MySaj < Oj::Saj
  def initialize
    @vec = []
    @label = ''
    @db = SQLite3::Database.new 'fraud.db'
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

  def hash_end(key)
    # pp @vec
    # pp @label
    @db.execute("INSERT INTO vectors VALUES (?, ?)", [
      SQLite3::Blob.new(@vec.pack('e*')),
      @label
    ])
  end
end

# db = SQLite3::Database.new 'fraud.db'
# db.enable_load_extension(true)
# db.load_extension('./vendor/vec1/vec1')
# db.execute <<-SQL
#      CREATE VIRTUAL TABLE IF NOT EXISTS vectors USING vec1(vector, is_fraud)
# SQL

parser = Oj::Parser.new(:saj)
parser.handler = MySaj.new
Zlib::GzipReader.open('resources/references.json.gz') do |gz|
  parser.load(gz)
end


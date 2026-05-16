# frozen_string_literal: true

require 'oj'
require 'zlib'

class MySaj < Oj::Saj
  def initialize
    @vec = []
    @label = ''
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
    pp @vec
    pp @label
  end
end

parser = Oj::Parser.new(:saj)
parser.handler = MySaj.new
Zlib::GzipReader.open('resources/references.json.gz') do |gz|
  parser.load(gz)
end


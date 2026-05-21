require 'oj'

Oj.default_options = { mode: :strict, symbol_keys: false }

class App
  READY_RESPONSE = [200, { 'content-type' => 'text/plain' }.freeze, ['ok'.freeze]].freeze
  NOT_FOUND      = [404, { 'content-type' => 'text/plain' }.freeze, ['not found'.freeze]].freeze
  BAD_REQUEST    = [400, { 'content-type' => 'text/plain' }.freeze, ['bad request'.freeze]].freeze
  JSON_HEADERS   = { 'content-type' => 'application/json' }.freeze

  def call(env)
    case env['REQUEST_METHOD']
    when 'GET'
      return READY_RESPONSE if env['PATH_INFO'] == '/ready'
    when 'POST'
      return fraud_score(env) if env['PATH_INFO'] == '/fraud-score'
    end

    NOT_FOUND
  end

  private

  def fraud_score(env)
    payload = Oj.load(env['rack.input'].read)

    response = { 'approved' => true, 'fraud_score' => 0.0 }

    [200, JSON_HEADERS, [Oj.dump(response)]]
  rescue Oj::ParseError
    BAD_REQUEST
  end
end

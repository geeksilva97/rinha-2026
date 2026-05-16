require 'oj'
require_relative 'lib/to_vec'

Oj.default_options = { mode: :strict }

run do |env|
  case [env['REQUEST_METHOD'], env['PATH_INFO']]
  when ['GET', '/ready']
    [200, {}, ['ok']]

  when ['POST', '/fraud-score']
    body = env['rack.input'].read
    payload = Oj.load(body)

    [200, {}, ['fraud-score']]
  else
    [404, {}, ['not found']]
  end
end

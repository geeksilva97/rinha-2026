if (sock = ENV['SOCK'])
  bind "unix://#{sock}?umask=0"
else
  port ENV.fetch('PORT', 9999)
end

workers Integer(ENV.fetch('WEB_CONCURRENCY', 1))
threads_count = Integer(ENV.fetch('RACK_MAX_THREADS', 4))
threads threads_count, threads_count

preload_app!
environment ENV.fetch('RACK_ENV', 'production')

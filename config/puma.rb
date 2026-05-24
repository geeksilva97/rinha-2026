if (sock = ENV['SOCK'])
  bind "unix://#{sock}?umask=0"
else
  port ENV.fetch('PORT', 9999)
end

# Single-mode (workers=0): no master/fork overhead — saves ~30-50MB in
# a tight 160MB cgroup. Cluster mode with 1 worker is the worst of both
# worlds (Puma itself warns about it).
workers Integer(ENV.fetch('WEB_CONCURRENCY', 0))
threads_count = Integer(ENV.fetch('RACK_MAX_THREADS', 4))
threads threads_count, threads_count

environment ENV.fetch('RACK_ENV', 'production')

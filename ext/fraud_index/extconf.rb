require 'mkmf'

# Point at the IVF index header
native_dir = File.expand_path('../../native', __dir__)
$INCFLAGS << " -I#{native_dir}"

# C++ flags
$CXXFLAGS << ' -std=c++26 -O2 -Wall'
# Debug symbols + frame pointer for perf/profiling.
# Enable via PROFILE=1 to avoid bloating the production .so.
$CXXFLAGS << ' -g -fno-omit-frame-pointer' if ENV['PROFILE'] == '1'

# -march=haswell enables AVX2 + FMA + BMI2 (Mac mini 2014, rinha target).
# Can break under Rosetta/QEMU (which don't emulate AVX2). Override via env:
#   CXX_MARCH=x86-64 → SSE2 baseline (works on any x86_64)
#   CXX_MARCH=haswell → AVX2 (prod default)
march = ENV.fetch('CXX_MARCH', 'haswell')
if RbConfig::CONFIG['host_cpu'] =~ /x86_64|amd64/
  $CXXFLAGS << " -march=#{march}"
end

# Ractor-safe opt-in (Ruby 3.0+). Probe so older Rubies still compile.
have_func('rb_ext_ractor_safe', 'ruby.h')

create_makefile('fraud_index/fraud_index')

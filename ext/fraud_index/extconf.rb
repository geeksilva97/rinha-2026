require 'mkmf'

# Aponta pro header do IVF index
native_dir = File.expand_path('../../native', __dir__)
$INCFLAGS << " -I#{native_dir}"

# C++ flags
$CXXFLAGS << ' -std=c++26 -O2 -Wall'

# -march=haswell ativa AVX2 + FMA + BMI2 (Mac mini 2014, alvo da rinha).
# Pode quebrar sob Rosetta/QEMU (que não emulam AVX2). Override via env:
#   CXX_MARCH=x86-64 → baseline SSE2 (funciona em qualquer x86_64)
#   CXX_MARCH=haswell → AVX2 (default em prod)
march = ENV.fetch('CXX_MARCH', 'haswell')
if RbConfig::CONFIG['host_cpu'] =~ /x86_64|amd64/
  $CXXFLAGS << " -march=#{march}"
end

create_makefile('fraud_index/fraud_index')

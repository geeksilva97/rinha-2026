require 'mkmf'

# Aponta pro header do IVF index
native_dir = File.expand_path('../../native', __dir__)
$INCFLAGS << " -I#{native_dir}"

# C++ flags
$CXXFLAGS << ' -std=c++26 -O2 -Wall'

create_makefile('fraud_index/fraud_index')

// Standalone IVF driver: loads ivf.bin, computes the score for a hardcoded
// query, and prints the top-5 plus the verdict.

#include "ivf_index.hpp"
#include <iostream>

using namespace ivf;
using namespace std;

// payload tx-1329056812 (last_transaction = null)
alignas(64) float QUERY[14] = {
    0.004112f,  0.166667f,  0.050000f, 0.782609f, 0.333333f,
    -1.000000f, -1.000000f, 0.029233f, 0.150000f, 0.000000f,
    1.000000f,  0.000000f,  0.150000f, 0.006025f,
};

int main(void) {
  IvfIndex idx;
  if (!load_index("ivf.bin", idx)) return 1;

  cout << "K=" << idx.K << "  D=" << idx.D << "  N=" << idx.N << endl;

  int nprobe = 1;
  alignas(32) int16_t q16[STRIDE];
  quantize_query(QUERY, idx.scale, q16);
  float fraud_score = ivf_score(idx, q16, nprobe);
  bool  approved    = fraud_score < 0.6f;

  cout << "nprobe=" << nprobe
       << "  fraud_score: " << fraud_score
       << "  approved: " << (approved ? "true" : "false") << endl;

  unload_index(idx);
  return 0;
}

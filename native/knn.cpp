#include "vendor/json.hpp"
#include <experimental/simd>
#include <fstream>
#include <iostream>
#include <random>

using namespace std;
using json = nlohmann::json;

std::mt19937 rng(42); // seed

int main(int argc, char *argv[]) {
  if (argc < 2) {
    std::cerr << "uso: " << argv[0] << " <references.json>\n";
    return 1;
  }
  std::string path = argv[1];
  std::ifstream f(path);
  json data = json::parse(f);
  std::vector<size_t> indices(data.size());
  std::iota(indices.begin(), indices.end(), 0); // 0,1,2,...,N-1

  size_t k = 1700;
  std::vector<size_t> sample;
  sample.reserve(k);
  std::sample(indices.begin(), indices.end(), std::back_inserter(sample), k,
              rng);

  // sample agora tem k índices únicos
  for (auto idx : sample) {
    cout << "Indice " << idx << endl;
    // data[idx]["vector"] é teu centróide inicial
  }

  cout << "Total: " << data.size() << endl;

  // for(const auto& item : data) {
  //   std::vector<float> vec = item["vector"];
  //   short label = item["vector"] == "legit" ? 0 : 1;
  // }

  return 0;
}

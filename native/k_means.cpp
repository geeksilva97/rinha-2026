#include "vendor/json.hpp"
#include <cstdint>
#include <experimental/simd>
#include <fstream>
#include <iostream>
#include <random>

using namespace std;
using json = nlohmann::json;

std::mt19937 rng(42); // seed

#define K 1700
#define T 20
#define DIM 14

int main(int argc, char *argv[]) {
  if (argc < 2) {
    std::cerr << "uso: " << argv[0] << " <references.json>\n";
    return 1;
  }

  std::string path = argv[1];
  std::ifstream f(path);
  json data = json::parse(f);
  std::vector<std::vector<float>> points;
  points.reserve(data.size());
  for (auto &item : data)
    points.push_back(item["vector"]);

  std::vector<size_t> assignments(data.size());
  // std::iota(indices.begin(), indices.end(), 0);
  // std::vector<size_t> centroids;
  //
  std::vector<std::vector<float>> centroids(K);
  std::sample(points.begin(), points.end(), centroids.begin(), K, rng);

  for (int t = 0; t < T; ++t) {
    // ASSIGN
    for (int i = 0; i < data.size(); ++i) {
      float best_dist = std::numeric_limits<float>::max();
      int best_j = 0;

      for (int j = 0; j < K; ++j) {
        float dist = 0;

        // this can probably be simd (?)
        for (int d = 0; d < DIM; ++d) {
          float diff = points[i][d] - centroids[j][d];
          dist += diff * diff;
        }

        if (dist < best_dist) {
          best_dist = dist;
          best_j = j;
        }
      }

      assignments[i] = best_j;
    }

    // UPDATE
    std::vector<std::vector<float>> sum(K, std::vector<float>(DIM, 0.0f));
    std::vector<int> count(K, 0);

    for (int i = 0; i < data.size(); ++i) {
      int c = assignments[i]; // cluster k
      for (int d = 0; d < DIM; ++d)
        sum[c][d] += points[i][d];
      count[c]++;
    }

    for (int j = 0; j < K; ++j) {
      if (count[j] == 0)
        continue; // orphan centroid
      for (int d = 0; d < DIM; ++d) {
        // updating centroid
        centroids[j][d] = sum[j][d] / count[j];
      }
    }
  }

  for (int j = 0; j < K; ++j) {
    std::cout << "centroide " << j << ": [";
    for (int d = 0; d < DIM; ++d)
      std::cout << centroids[j][d] << " ";
    std::cout << "]\n";
  }

  return 0;
}

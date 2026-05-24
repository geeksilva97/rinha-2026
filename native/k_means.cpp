#include "vendor/json.hpp"
#include <bit>
#include <experimental/simd>
#include <fstream>
#include <iostream>
#include <random>

static_assert(std::endian::native == std::endian::little,
              "this code assumes little-endian");

using namespace std;
using json = nlohmann::json;

std::mt19937 rng(42); // seed

constexpr uint32_t K = 1700;
constexpr uint32_t T = 20;
constexpr uint32_t DIM = 14;

int main(int argc, char *argv[]) {
  if (argc < 2) {
    std::cerr << "uso: " << argv[0] << " <references.json>\n";
    return 1;
  }

  std::string path = argv[1];
  std::ifstream f(path);
  json data = json::parse(f);
  std::vector<std::vector<float>> points;
  std::vector<uint8_t> labels;
  points.reserve(data.size());
  labels.reserve(data.size());
  for (auto &item : data) {
    points.push_back(item["vector"]);
    labels.push_back(item["label"] == "fraud" ? 1 : 0);
  }

  std::vector<size_t> assignments(data.size());
  std::vector<std::vector<float>> centroids(K);
  std::sample(points.begin(), points.end(), centroids.begin(), K, rng);
  std::vector<std::vector<float>> sum(K, std::vector<float>(DIM, 0.0f));
  std::vector<int> count(K, 0);

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
    std::fill(count.begin(), count.end(), 0);
    for (auto &s : sum)
      std::fill(s.begin(), s.end(), 0.0f);
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

  // save file
  ofstream out("ivf.bin", ios::binary);

  if (!out) {
    cerr << "Error while opening the file for writing";
    return 1;
  }

  uint32_t N = static_cast<uint32_t>(data.size());

  // HEADER
  out.write(reinterpret_cast<const char *>(&K), sizeof(K));
  out.write(reinterpret_cast<const char *>(&DIM), sizeof(DIM));
  out.write(reinterpret_cast<const char *>(&N), sizeof(N));

  // CENTROIDS (K * DIM floats)
  for (const auto &centroid : centroids) {
    out.write(reinterpret_cast<const char *>(centroid.data()),
              DIM * sizeof(float));
  }

  // OFFSETS (prefix-sum dos counts)
  std::vector<uint32_t> offsets(K + 1);
  offsets[0] = 0;
  for (uint32_t j = 0; j < K; ++j) {
    offsets[j + 1] = offsets[j] + count[j];
  }
  out.write(reinterpret_cast<const char *>(offsets.data()),
            offsets.size() * sizeof(uint32_t));

  // VECTORS ordenados por cluster (via cursor)
  auto cursor = offsets;
  std::vector<float> ordered_vectors(N * DIM);
  std::vector<uint8_t> ordered_labels(N);

  for (uint32_t i = 0; i < N; ++i) {
    uint32_t cluster_index = assignments[i];
    uint32_t pos = cursor[cluster_index]++;

    for (uint32_t d = 0; d < DIM; ++d)
      ordered_vectors[pos * DIM + d] = points[i][d];

    ordered_labels[pos] = labels[i];
  }

  out.write(reinterpret_cast<const char *>(ordered_vectors.data()),
            ordered_vectors.size() * sizeof(float));

  // LABELS
  out.write(reinterpret_cast<const char *>(ordered_labels.data()),
            ordered_labels.size() * sizeof(uint8_t));

  out.close();

  return 0;
}

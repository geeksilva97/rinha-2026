#include "ivf_index.hpp"  // ivf::dist_sq (SIMD when available)
#include "vendor/json.hpp"
#include <bit>
#include <chrono>
#include <fstream>
#include <iostream>
#include <random>

static_assert(std::endian::native == std::endian::little,
              "this code assumes little-endian");

using namespace std;
using json = nlohmann::json;
using ivf::dist_sq;

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

  using clock = std::chrono::steady_clock;
  auto t_start = clock::now();

  cout << "training k-means: K=" << K << " T=" << T << " DIM=" << DIM
       << " N=" << data.size() << "\n";

  for (uint32_t t = 0; t < T; ++t) {
    auto iter_start = clock::now();

    // ASSIGN (hot loop: N × K × D ops; dist_sq uses SIMD when available)
    for (size_t i = 0; i < data.size(); ++i) {
      float best_dist = std::numeric_limits<float>::max();
      int best_j = 0;
      const float *xi = points[i].data();

      for (uint32_t j = 0; j < K; ++j) {
        float dist = dist_sq(xi, centroids[j].data());
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
    for (size_t i = 0; i < data.size(); ++i) {
      int c = assignments[i];
      for (uint32_t d = 0; d < DIM; ++d)
        sum[c][d] += points[i][d];
      count[c]++;
    }

    for (uint32_t j = 0; j < K; ++j) {
      if (count[j] == 0) continue; // orphan centroid
      for (uint32_t d = 0; d < DIM; ++d) {
        centroids[j][d] = sum[j][d] / count[j];
      }
    }

    auto iter_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        clock::now() - iter_start).count();
    auto total_s = std::chrono::duration_cast<std::chrono::seconds>(
        clock::now() - t_start).count();
    cout << "  iter " << (t + 1) << "/" << T
         << "  took=" << iter_ms << "ms"
         << "  total=" << total_s << "s\n" << std::flush;
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

  // VECTORS sorted by cluster (using cursor array)
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

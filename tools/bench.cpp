// bench.cpp — measurement driver for the sieve-lab experiments.
#include "sieve_core.hpp"
#include "sieve_parallel.hpp"
#include "wheel_table.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace {

double parse_num(const char* s) {
  std::string_view v(s);
  double scale = 1.0;
  if (!v.empty() && (v.back() == 'k' || v.back() == 'K')) {
    scale = 1e3; v.remove_suffix(1);
  } else if (!v.empty() && (v.back() == 'm' || v.back() == 'M')) {
    scale = 1e6; v.remove_suffix(1);
  } else if (!v.empty() && (v.back() == 'g' || v.back() == 'G')) {
    scale = 1e9; v.remove_suffix(1);
  }
  return std::strtod(std::string(v).c_str(), nullptr) * scale;
}

void usage() {
  std::printf("bench --n <n> [--wheel 2|30|210] [--bucket 0|1] "
              "[--seg-kb <kb>] [--reps <k>] [--threads <t>] [--header]\n");
}

} // namespace

int main(int argc, char** argv) {
  std::uint64_t n = 0;
  sielab::Options opts;
  int reps = 3;
  std::uint32_t threads = 1;
  bool header = false;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--n") && i + 1 < argc) n = static_cast<std::uint64_t>(parse_num(argv[++i]));
    else if (!std::strcmp(argv[i], "--wheel") && i + 1 < argc) opts.wheel = static_cast<std::uint32_t>(std::atoi(argv[++i]));
    else if (!std::strcmp(argv[i], "--bucket") && i + 1 < argc) opts.bucket = std::atoi(argv[++i]) != 0;
    else if (!std::strcmp(argv[i], "--seg-kb") && i + 1 < argc) opts.segment_bytes = static_cast<std::size_t>(std::atoi(argv[++i])) * 1024;
    else if (!std::strcmp(argv[i], "--reps") && i + 1 < argc) reps = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--threads") && i + 1 < argc) threads = static_cast<std::uint32_t>(std::atoi(argv[++i]));
    else if (!std::strcmp(argv[i], "--header")) header = true;
    else { usage(); return 2; }
  }
  if (!n || reps < 1) { usage(); return 2; }

  if (header) {
    std::printf("limit,wheel,bucket,segment_kb,reps,seconds,primes,segments,marks,threads\n");
    return 0;
  }

  sielab::Stats st;
  double best = 1e100;
  std::uint64_t primes = 0;
  for (int r = 0; r < reps; ++r) {
    if (threads > 1) sielab::count_primes_parallel(n, opts, threads, &st);
    else sielab::count_primes(n, opts, &st);
    if (st.seconds < best) best = st.seconds;
    primes = st.primes;
  }
  std::printf("%llu,%u,%d,%zu,%d,%.4f,%llu,%zu,%llu,%u\n",
              static_cast<unsigned long long>(n), opts.wheel, opts.bucket ? 1 : 0,
              opts.segment_bytes / 1024, reps, best,
              static_cast<unsigned long long>(primes), st.segments,
              static_cast<unsigned long long>(st.marks), threads);
  return 0;
}
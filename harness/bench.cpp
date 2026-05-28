//===----------------------------------------------------------------------===//
//
// Two modes, selected on the command line:
//
//   (no args)            Timing mode.  Runs every builtin across every input
//                        category and prints, tab-separated:
//                          <builtin>\t<category>\t<median_ns>\t<p25>\t<p75>
//
//   --count NAME CAT N   Instruction-count mode.  Runs exactly N iterations of
//                        the NAME builtin on CAT inputs, with no timing and no
//                        warmup, then exits.  Wrap this in `perf stat -e
//                        instructions` and subtract two runs (N and 2N) to get
//                        an overhead-free per-op instruction count.
//
// The same binary is linked twice -- against the legacy and the libc-backed
// libclang_rt.builtins.a -- so the two can be compared on identical inputs.
//===----------------------------------------------------------------------===//

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

extern "C" __float128 __addtf3(__float128, __float128);
extern "C" __float128 __subtf3(__float128, __float128);
extern "C" __float128 __multf3(__float128, __float128);
extern "C" __float128 __divtf3(__float128, __float128);
extern "C" double __adddf3(double, double);
extern "C" double __subdf3(double, double);
extern "C" double __muldf3(double, double);
extern "C" double __divdf3(double, double);

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t N_INPUTS = 1u << 14;
constexpr int N_RUNS = 11;
constexpr long long TIME_ITERS = 8'000'000;

template <typename T> struct FpTraits;
template <> struct FpTraits<double> {
  using UInt = std::uint64_t;
  static constexpr int FRAC_BITS = 52, EXP_BITS = 11;
};
template <> struct FpTraits<__float128> {
  using UInt = unsigned __int128;
  static constexpr int FRAC_BITS = 112, EXP_BITS = 15;
};

template <typename T> using Op = T(T, T);
template <typename T> using UIntOf = typename FpTraits<T>::UInt;

template <typename T> T from_bits(UIntOf<T> b) {
  T x;
  std::memcpy(&x, &b, sizeof(x));
  return x;
}
template <typename T> UIntOf<T> to_bits(T x) {
  UIntOf<T> b;
  std::memcpy(&b, &x, sizeof(b));
  return b;
}
template <typename T> UIntOf<T> frac_mask() {
  return (UIntOf<T>(1) << FpTraits<T>::FRAC_BITS) - 1;
}
template <typename T> UIntOf<T> rand_frac(std::mt19937_64 &rng) {
  using U = UIntOf<T>;
  U v = static_cast<U>(rng());
  if constexpr (sizeof(U) > 8)
    v = (v << 64) | static_cast<U>(rng());
  return v & frac_mask<T>();
}

template <typename T> struct Inputs {
  std::vector<T> xs, ys;
};

template <typename T> Inputs<T> make_normal(std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::uniform_real_distribution<double> d(-1e100, 1e100);
  Inputs<T> in;
  in.xs.resize(N_INPUTS);
  in.ys.resize(N_INPUTS);
  for (std::size_t i = 0; i < N_INPUTS; ++i) {
    in.xs[i] = static_cast<T>(d(rng));
    T y = static_cast<T>(d(rng));
    in.ys[i] = (y == static_cast<T>(0)) ? static_cast<T>(1.5) : y;
  }
  return in;
}

template <typename T> Inputs<T> make_subnormal(std::uint64_t seed) {
  using U = UIntOf<T>;
  constexpr int SIGN = 8 * sizeof(T) - 1;
  std::mt19937_64 rng(seed);
  Inputs<T> in;
  in.xs.resize(N_INPUTS);
  in.ys.resize(N_INPUTS);
  for (std::size_t i = 0; i < N_INPUTS; ++i) {
    in.xs[i] = from_bits<T>((U(rng() & 1) << SIGN) | (rand_frac<T>(rng) | 1));
    in.ys[i] = from_bits<T>((U(rng() & 1) << SIGN) | (rand_frac<T>(rng) | 1));
  }
  return in;
}

template <typename T> Inputs<T> make_special(std::uint64_t seed) {
  using U = UIntOf<T>;
  constexpr int FB = FpTraits<T>::FRAC_BITS, SIGN = 8 * sizeof(T) - 1;
  const U exp_inf = ((U(1) << FpTraits<T>::EXP_BITS) - 1) << FB;
  const U qnan = U(1) << (FB - 1);
  std::mt19937_64 rng(seed);
  Inputs<T> in;
  in.xs.resize(N_INPUTS);
  in.ys.resize(N_INPUTS);
  for (std::size_t i = 0; i < N_INPUTS; ++i) {
    U fx = (i & 1) ? (rand_frac<T>(rng) | qnan) : U(0);
    U fy = (i & 1) ? U(0) : (rand_frac<T>(rng) | qnan);
    in.xs[i] = from_bits<T>((U(rng() & 1) << SIGN) | exp_inf | fx);
    in.ys[i] = from_bits<T>((U(rng() & 1) << SIGN) | exp_inf | fy);
  }
  return in;
}

template <typename T> Inputs<T> make_cancellation(std::uint64_t seed) {
  using U = UIntOf<T>;
  constexpr int SIGN = 8 * sizeof(T) - 1;
  std::mt19937_64 rng(seed);
  std::uniform_real_distribution<double> d(-1e100, 1e100);
  Inputs<T> in;
  in.xs.resize(N_INPUTS);
  in.ys.resize(N_INPUTS);
  for (std::size_t i = 0; i < N_INPUTS; ++i) {
    T x = static_cast<T>(d(rng));
    if (x == static_cast<T>(0))
      x = static_cast<T>(1.5);
    in.xs[i] = x;
    U yb = to_bits<T>(x) ^ (U(1) << SIGN);
    yb ^= (rng() & 0xFF);
    in.ys[i] = from_bits<T>(yb);
  }
  return in;
}

template <typename T>
Inputs<T> gen(const std::string &cat, std::uint64_t seed) {
  if (cat == "normal")
    return make_normal<T>(seed);
  if (cat == "subnormal")
    return make_subnormal<T>(seed);
  if (cat == "special")
    return make_special<T>(seed);
  if (cat == "cancellation")
    return make_cancellation<T>(seed);
  std::fprintf(stderr, "unknown category: %s\n", cat.c_str());
  std::exit(2);
}

// The measured inner loop, shared by timing and count modes.
template <typename T>
UIntOf<T> spin(Op<T> *op, const Inputs<T> &in, long long n) {
  UIntOf<T> sbits = 0;
  for (long long i = 0; i < n; ++i) {
    std::size_t idx = static_cast<std::size_t>(i) & (N_INPUTS - 1);
    T r = op(in.xs[idx], in.ys[idx]);
    asm volatile("" : "+m"(r));
    UIntOf<T> rb;
    std::memcpy(&rb, &r, sizeof(rb));
    sbits ^= rb;
  }
  return sbits;
}

template <typename T> double time_once(Op<T> *op, const Inputs<T> &in) {
  auto t0 = Clock::now();
  UIntOf<T> s = spin<T>(op, in, TIME_ITERS);
  auto t1 = Clock::now();
  asm volatile("" ::"r"(static_cast<std::uint64_t>(s)));
  return std::chrono::duration<double, std::nano>(t1 - t0).count() /
         static_cast<double>(TIME_ITERS);
}

template <typename T>
void time_bench(const char *name, Op<T> *op, const char *cat,
                const Inputs<T> &in) {
  time_once<T>(op, in); // warmup
  std::vector<double> s;
  s.reserve(N_RUNS);
  for (int i = 0; i < N_RUNS; ++i)
    s.push_back(time_once<T>(op, in));
  std::sort(s.begin(), s.end());
  std::printf("%s\t%s\t%.3f\t%.3f\t%.3f\n", name, cat, s[N_RUNS / 2],
              s[N_RUNS / 4], s[(3 * N_RUNS) / 4]);
  std::fflush(stdout);
}

template <typename T> struct Builtin {
  const char *name;
  Op<T> *op;
};

const Builtin<__float128> QUAD[] = {
    {"__addtf3", &__addtf3},
    {"__subtf3", &__subtf3},
    {"__multf3", &__multf3},
    {"__divtf3", &__divtf3},
};
const Builtin<double> DBL[] = {
    {"__adddf3", &__adddf3},
    {"__subdf3", &__subdf3},
    {"__muldf3", &__muldf3},
    {"__divdf3", &__divdf3},
};

const char *CATEGORIES[] = {"normal", "subnormal", "special", "cancellation"};

// Which builtins are enabled.  Edit to match what the libc archive provides.
const char *ENABLED[] = {"__addtf3", "__adddf3"};

bool enabled(const char *name) {
  for (const char *e : ENABLED)
    if (std::strcmp(e, name) == 0)
      return true;
  return false;
}

int run_count(const std::string &name, const std::string &cat,
              long long iters) {
  std::uint64_t seed = 0xC0FFEEull;
  for (const auto &b : QUAD)
    if (name == b.name) {
      Inputs<__float128> in = gen<__float128>(cat, seed);
      asm volatile("" ::"r"(static_cast<std::uint64_t>(spin(b.op, in, iters))));
      return 0;
    }
  for (const auto &b : DBL)
    if (name == b.name) {
      Inputs<double> in = gen<double>(cat, seed);
      asm volatile("" ::"r"(static_cast<std::uint64_t>(spin(b.op, in, iters))));
      return 0;
    }
  std::fprintf(stderr, "unknown builtin: %s\n", name.c_str());
  return 2;
}

void run_timing() {
  std::printf("builtin\tcategory\tmedian_ns\tp25_ns\tp75_ns\n");
  std::uint64_t seed = 0xC0FFEEull;
  for (const char *cat : CATEGORIES) {
    Inputs<__float128> q = gen<__float128>(cat, seed);
    Inputs<double> d = gen<double>(cat, seed);
    ++seed;
    for (const auto &b : QUAD)
      if (enabled(b.name))
        time_bench(b.name, b.op, cat, q);
    for (const auto &b : DBL)
      if (enabled(b.name))
        time_bench(b.name, b.op, cat, d);
  }
}

} // namespace

int main(int argc, char **argv) {
  if (argc >= 5 && std::strcmp(argv[1], "--count") == 0)
    return run_count(argv[2], argv[3], std::atoll(argv[4]));
  run_timing();
  return 0;
}

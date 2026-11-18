// Throwaway exact-kNN scan microbenchmark for docs/design/hybrid-search.md.
// One thread, top-10 by dot product (cosine over unit vectors), over f32, f16, int8, and
// binary codes; hot (one index re-queried) and cold (a different index copy each query,
// total well past the LLC), and filtered (score only a sorted subset of ordinals).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>
#if defined(__AVX2__)
#include <immintrin.h>
#elif defined(__ARM_NEON)
#include <arm_neon.h>
#endif

using u8 = std::uint8_t; using i8 = std::int8_t; using u16 = std::uint16_t; using u64 = std::uint64_t;

static float dot_f32_scalar(const float* a, const float* b, int d) {
	float s = 0; for (int i = 0; i < d; ++i) s += a[i] * b[i]; return s;
}
static float dot_f32(const float* a, const float* b, int d) {
#if defined(__AVX2__)
	__m256 s0 = _mm256_setzero_ps(), s1 = s0, s2 = s0, s3 = s0;
	int i = 0;
	for (; i + 32 <= d; i += 32) {
		s0 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i), s0);
		s1 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 8), _mm256_loadu_ps(b + i + 8), s1);
		s2 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 16), _mm256_loadu_ps(b + i + 16), s2);
		s3 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 24), _mm256_loadu_ps(b + i + 24), s3);
	}
	__m256 s = _mm256_add_ps(_mm256_add_ps(s0, s1), _mm256_add_ps(s2, s3));
	__m128 h = _mm_add_ps(_mm256_castps256_ps128(s), _mm256_extractf128_ps(s, 1));
	h = _mm_hadd_ps(h, h); h = _mm_hadd_ps(h, h);
	float r = _mm_cvtss_f32(h);
	for (; i < d; ++i) r += a[i] * b[i];
	return r;
#elif defined(__ARM_NEON)
	float32x4_t s0 = vdupq_n_f32(0), s1 = s0, s2 = s0, s3 = s0;
	int i = 0;
	for (; i + 16 <= d; i += 16) {
		s0 = vfmaq_f32(s0, vld1q_f32(a + i), vld1q_f32(b + i));
		s1 = vfmaq_f32(s1, vld1q_f32(a + i + 4), vld1q_f32(b + i + 4));
		s2 = vfmaq_f32(s2, vld1q_f32(a + i + 8), vld1q_f32(b + i + 8));
		s3 = vfmaq_f32(s3, vld1q_f32(a + i + 12), vld1q_f32(b + i + 12));
	}
	float r = vaddvq_f32(vaddq_f32(vaddq_f32(s0, s1), vaddq_f32(s2, s3)));
	for (; i < d; ++i) r += a[i] * b[i];
	return r;
#else
	return dot_f32_scalar(a, b, d);
#endif
}
// f16 codes against an f32 query.
static float dot_f16(const u16* a, const float* b, int d) {
#if defined(__AVX2__)
	__m256 s0 = _mm256_setzero_ps(), s1 = s0;
	int i = 0;
	for (; i + 16 <= d; i += 16) {
		s0 = _mm256_fmadd_ps(_mm256_cvtph_ps(_mm_loadu_si128((const __m128i*)(a + i))), _mm256_loadu_ps(b + i), s0);
		s1 = _mm256_fmadd_ps(_mm256_cvtph_ps(_mm_loadu_si128((const __m128i*)(a + i + 8))), _mm256_loadu_ps(b + i + 8), s1);
	}
	__m256 s = _mm256_add_ps(s0, s1);
	__m128 h = _mm_add_ps(_mm256_castps256_ps128(s), _mm256_extractf128_ps(s, 1));
	h = _mm_hadd_ps(h, h); h = _mm_hadd_ps(h, h);
	return _mm_cvtss_f32(h);
#elif defined(__ARM_NEON)
	float32x4_t s0 = vdupq_n_f32(0), s1 = s0;
	int i = 0;
	for (; i + 8 <= d; i += 8) {
		float16x8_t x = vreinterpretq_f16_u16(vld1q_u16(a + i));
		s0 = vfmaq_f32(s0, vcvt_f32_f16(vget_low_f16(x)), vld1q_f32(b + i));
		s1 = vfmaq_f32(s1, vcvt_f32_f16(vget_high_f16(x)), vld1q_f32(b + i + 4));
	}
	return vaddvq_f32(vaddq_f32(s0, s1));
#else
	(void)a; (void)b; (void)d; return 0;
#endif
}
static std::int32_t dot_i8(const i8* a, const i8* b, int d) {
#if defined(__AVX2__)
	__m256i s0 = _mm256_setzero_si256(), s1 = s0;
	int i = 0;
	for (; i + 32 <= d; i += 32) {
		__m256i a0 = _mm256_cvtepi8_epi16(_mm_loadu_si128((const __m128i*)(a + i)));
		__m256i b0 = _mm256_cvtepi8_epi16(_mm_loadu_si128((const __m128i*)(b + i)));
		__m256i a1 = _mm256_cvtepi8_epi16(_mm_loadu_si128((const __m128i*)(a + i + 16)));
		__m256i b1 = _mm256_cvtepi8_epi16(_mm_loadu_si128((const __m128i*)(b + i + 16)));
		s0 = _mm256_add_epi32(s0, _mm256_madd_epi16(a0, b0));
		s1 = _mm256_add_epi32(s1, _mm256_madd_epi16(a1, b1));
	}
	__m256i s = _mm256_add_epi32(s0, s1);
	__m128i h = _mm_add_epi32(_mm256_castsi256_si128(s), _mm256_extracti128_si256(s, 1));
	h = _mm_hadd_epi32(h, h); h = _mm_hadd_epi32(h, h);
	return _mm_cvtsi128_si32(h);
#elif defined(__ARM_NEON)
	int32x4_t s = vdupq_n_s32(0);
	for (int i = 0; i + 16 <= d; i += 16) {
		int8x16_t x = vld1q_s8(a + i), y = vld1q_s8(b + i);
		s = vpadalq_s16(s, vmull_s8(vget_low_s8(x), vget_low_s8(y)));
		s = vpadalq_s16(s, vmull_s8(vget_high_s8(x), vget_high_s8(y)));
	}
	return vaddvq_s32(s);
#else
	std::int32_t s = 0; for (int i = 0; i < d; ++i) s += a[i] * b[i]; return s;
#endif
}
static int hamming(const u64* a, const u64* b, int words) {
	int s0 = 0, s1 = 0, s2 = 0, s3 = 0, i = 0;
	for (; i + 4 <= words; i += 4) {
		s0 += __builtin_popcountll(a[i] ^ b[i]); s1 += __builtin_popcountll(a[i + 1] ^ b[i + 1]);
		s2 += __builtin_popcountll(a[i + 2] ^ b[i + 2]); s3 += __builtin_popcountll(a[i + 3] ^ b[i + 3]);
	}
	for (; i < words; ++i) s0 += __builtin_popcountll(a[i] ^ b[i]);
	return s0 + s1 + s2 + s3;
}

struct topk {
	int k; std::vector<float> s; std::vector<std::uint32_t> id; float min = -INFINITY; int n = 0;
	explicit topk(int k_) : k(k_), s(k_), id(k_) {}
	void reset() { n = 0; min = -INFINITY; }
	inline void push(float v, std::uint32_t o) {
		if (n < k) { s[n] = v; id[n] = o; ++n; if (n == k) min = *std::min_element(s.begin(), s.end()); return; }
		if (v <= min) return;
		int m = int(std::min_element(s.begin(), s.end()) - s.begin());
		s[m] = v; id[m] = o; min = *std::min_element(s.begin(), s.end());
	}
};

enum class enc { f32_scalar, f32, f16, i8, bin };
static const char* name(enc e) { switch (e) { case enc::f32_scalar: return "f32_scalar"; case enc::f32: return "f32"; case enc::f16: return "f16"; case enc::i8: return "int8"; case enc::bin: return "binary"; } return "?"; }
static std::size_t code_bytes(enc e, int d) { switch (e) { case enc::f32_scalar: case enc::f32: return 4u * d; case enc::f16: return 2u * d; case enc::i8: return d; case enc::bin: return d / 8; } return 0; }

static u16 to_f16(float f) {
#if defined(__AVX2__)
	return (u16)_mm_extract_epi16(_mm_cvtps_ph(_mm_set_ss(f), 0), 0);
#elif defined(__ARM_NEON)
	__fp16 h = (__fp16)f; u16 r; std::memcpy(&r, &h, 2); return r;
#else
	(void)f; return 0;
#endif
}

// One index: n codes of one encoding, contiguous (as a segment column would hold them).
struct index_copy { std::vector<u8> bytes; };

static void fill(index_copy& ix, enc e, int n, int d, std::mt19937_64& rng) {
	std::size_t cb = code_bytes(e, d);
	ix.bytes.assign(cb * n, 0);
	std::uint64_t x = rng();
	auto nd = [&x](std::mt19937_64&) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return float(std::int64_t(x >> 11) - (std::int64_t(1) << 52)) / float(std::int64_t(1) << 52); };
	std::vector<float> v(d);
	for (int r = 0; r < n; ++r) {
		float norm = 0; for (auto& x : v) { x = nd(rng); norm += x * x; }
		norm = 1.0f / std::sqrt(norm); for (auto& x : v) x *= norm;
		u8* p = ix.bytes.data() + cb * r;
		switch (e) {
		case enc::f32_scalar: case enc::f32: std::memcpy(p, v.data(), cb); break;
		case enc::f16: for (int i = 0; i < d; ++i) { u16 h = to_f16(v[i]); std::memcpy(p + 2 * i, &h, 2); } break;
		case enc::i8: for (int i = 0; i < d; ++i) { float q = std::round(v[i] * 127.0f * 8.0f); q = std::clamp(q, -127.0f, 127.0f); ((i8*)p)[i] = (i8)q; } break;
		case enc::bin: for (int i = 0; i < d; ++i) if (v[i] > 0) p[i / 8] |= u8(1u << (i % 8)); break;
		}
	}
}

struct query { std::vector<float> f; std::vector<i8> q8; std::vector<u64> qb; };

// Scores a block of ordinals into a buffer, then offers the buffer to the top-k: the kernel
// loop carries no branch on the heap.
template <enc E>
static inline float score_one(const u8* p, const query& q, int d) {
	if constexpr (E == enc::f32_scalar) return dot_f32_scalar((const float*)p, q.f.data(), d);
	else if constexpr (E == enc::f32) return dot_f32((const float*)p, q.f.data(), d);
	else if constexpr (E == enc::f16) return dot_f16((const u16*)p, q.f.data(), d);
	else if constexpr (E == enc::i8) return float(dot_i8((const i8*)p, q.q8.data(), d));
	else return -float(hamming((const u64*)p, q.qb.data(), d / 64));
}
template <enc E>
static void scan_t(const index_copy& ix, int n, int d, const query& q, topk& t, const std::vector<std::uint32_t>* subset) {
	constexpr int B = 256;
	float buf[B];
	std::size_t cb = code_bytes(E, d);
	const u8* base = ix.bytes.data();
	std::size_t total = subset ? subset->size() : std::size_t(n);
	for (std::size_t at = 0; at < total; at += B) {
		int m = int(std::min<std::size_t>(B, total - at));
		if (subset) for (int j = 0; j < m; ++j) buf[j] = score_one<E>(base + cb * (*subset)[at + j], q, d);
		else for (int j = 0; j < m; ++j) buf[j] = score_one<E>(base + cb * (at + j), q, d);
		for (int j = 0; j < m; ++j) if (buf[j] > t.min || t.n < t.k) t.push(buf[j], subset ? (*subset)[at + j] : std::uint32_t(at + j));
	}
}
static void scan(const index_copy& ix, enc e, int n, int d, const query& q, topk& t, const std::vector<std::uint32_t>* subset) {
	switch (e) {
	case enc::f32_scalar: scan_t<enc::f32_scalar>(ix, n, d, q, t, subset); break;
	case enc::f32: scan_t<enc::f32>(ix, n, d, q, t, subset); break;
	case enc::f16: scan_t<enc::f16>(ix, n, d, q, t, subset); break;
	case enc::i8: scan_t<enc::i8>(ix, n, d, q, t, subset); break;
	case enc::bin: scan_t<enc::bin>(ix, n, d, q, t, subset); break;
	}
}

int main(int argc, char** argv) {
	double min_seconds = argc > 1 ? std::atof(argv[1]) : 0.3;
	std::size_t cold_bytes = std::size_t(argc > 2 ? std::atof(argv[2]) : 256) << 20;  // pool past the LLC
	const char* only = argc > 3 ? argv[3] : nullptr;
	std::mt19937_64 rng(42);
	const int dims[] = {384, 768, 1536};
	const int sizes[] = {100, 1000, 10000, 100000, 1000000};
	const enc encs[] = {enc::f32_scalar, enc::f32, enc::f16, enc::i8, enc::bin};
	const std::size_t max_index_bytes = std::size_t(1600) << 20;
	for (int d : dims) for (enc e : encs) for (int n : sizes) {
		index_copy base_ix; bool filled = false;
		for (int mode = 0; mode < 4; ++mode) {
		// mode 0 hot, 1 cold, 2 hot filtered 10%, 3 hot filtered 1%
		if (only && std::string(only) != name(e)) continue;
		if (e == enc::f32_scalar && (mode != 0 || n > 100000)) continue;
		std::size_t ib = code_bytes(e, d) * std::size_t(n);
		if (ib > max_index_bytes) continue;
		if (mode >= 2 && n < 1000) continue;
		int copies = mode == 1 ? int(std::max<std::size_t>(2, std::min<std::size_t>(4096, cold_bytes / std::max<std::size_t>(ib, 1)))) : 1;
		if (mode == 1 && ib >= cold_bytes) copies = 1;  // one index already past the LLC: cold is its own hot
		if (mode == 1 && copies == 1) continue;
		std::vector<index_copy> pool(copies);
		if (!filled) { fill(base_ix, e, n, d, rng); filled = true; }
		pool[0].bytes = base_ix.bytes;
		for (std::size_t c2 = 1; c2 < pool.size(); ++c2) pool[c2].bytes = pool[0].bytes;  // same bytes, distinct memory
		std::vector<std::uint32_t> subset;
		if (mode >= 2) {
			double frac = mode == 2 ? 0.10 : 0.01;
			std::bernoulli_distribution bd(frac);
			for (int o = 0; o < n; ++o) if (bd(rng)) subset.push_back(std::uint32_t(o));
		}
		query q; q.f.resize(d); q.q8.resize(d); q.qb.assign(d / 64, 0);
		{ std::normal_distribution<float> nd; float norm = 0; for (auto& x : q.f) { x = nd(rng); norm += x * x; } norm = 1 / std::sqrt(norm);
		  for (int i = 0; i < d; ++i) { q.f[i] *= norm; q.q8[i] = (i8)std::clamp(std::round(q.f[i] * 127.0f * 8.0f), -127.0f, 127.0f); if (q.f[i] > 0) q.qb[i / 64] |= u64(1) << (i % 64); } }
		topk t(10);
		std::vector<double> ns;
		auto t0 = std::chrono::steady_clock::now();
		int c = 0; double sink = 0;
		for (int w = 0; w < 2; ++w) { t.reset(); scan(pool[0], e, n, d, q, t, mode >= 2 ? &subset : nullptr); }
		while (true) {
			const index_copy& ix = pool[std::size_t(c) % pool.size()];
			auto a = std::chrono::steady_clock::now();
			t.reset(); scan(ix, e, n, d, q, t, mode >= 2 ? &subset : nullptr);
			auto b = std::chrono::steady_clock::now();
			sink += t.s[0];
			ns.push_back(std::chrono::duration<double, std::nano>(b - a).count());
			++c;
			double el = std::chrono::duration<double>(b - t0).count();
			if ((el > min_seconds && c >= 15) || c >= 200000) break;
		}
		std::sort(ns.begin(), ns.end());
		double med = ns[ns.size() / 2], p99 = ns[std::min(ns.size() - 1, ns.size() * 99 / 100)];
		std::size_t scored = mode >= 2 ? subset.size() : std::size_t(n);
		static const char* modes[] = {"hot", "cold", "filter10", "filter1"};
		std::printf("{\"enc\":\"%s\",\"d\":%d,\"n\":%d,\"mode\":\"%s\",\"scored\":%zu,\"queries\":%d,\"median_us\":%.3f,\"p99_us\":%.3f,"
		            "\"ns_per_vec\":%.3f,\"gbps\":%.3f,\"index_bytes\":%zu,\"copies\":%d,\"sink\":%.1f}\n",
		            name(e), d, n, modes[mode], scored, c, med / 1e3, p99 / 1e3, med / double(scored),
		            double(code_bytes(e, d)) * double(scored) / med, ib, copies, sink > 1e30 ? 1.0 : 0.0);
		std::fflush(stdout);
		}
	}
}

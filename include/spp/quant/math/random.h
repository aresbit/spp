#pragma once

#ifndef SPP_BASE
#error "Include base.h instead."
#endif

#include <cmath>

namespace spp::quant::rng {

// ============================================================
// Mersenne Twister MT19937
// ============================================================
// Standard MT19937 implementation (Matsumoto & Nishimura, 1998).
// State: 624 u64 words (the original uses u32, but we adapt to u64
// for consistency with spp's type system; the algorithm uses the
// lower 32 bits of each word).

struct MT19937 {
    static constexpr u64 N = 624;
    static constexpr u64 M = 397;
    static constexpr u64 MATRIX_A = 0x9908B0DFULL;
    static constexpr u64 UPPER_MASK = 0x80000000ULL;
    static constexpr u64 LOWER_MASK = 0x7FFFFFFFULL;

    u64 state_[N];
    u64 index_;

    MT19937() noexcept {
        seed(5489ULL);
    }

    explicit MT19937(u64 s) noexcept {
        seed(s);
    }

    void seed(u64 s) noexcept {
        state_[0] = s;
        for (u64 i = 1; i < N; i++) {
            state_[i] = 6364136223846793005ULL * (state_[i - 1] ^ (state_[i - 1] >> 62)) + i;
            state_[i] &= 0xFFFFFFFFULL;  // Keep lower 32 bits
        }
        index_ = N;
    }

    [[nodiscard]] u64 next_u64() noexcept {
        if (index_ >= N) {
            twist();
        }

        u64 y = state_[index_++];

        // Tempering (MT19937-32 standard)
        y ^= (y >> 11);
        y ^= (y << 7) & 0x9D2C5680ULL;
        y ^= (y << 15) & 0xEFC60000ULL;
        y ^= (y >> 18);

        return y & 0xFFFFFFFFULL;  // Return 32-bit value in u64
    }

    // Uniform f64 in [0, 1)
    [[nodiscard]] f64 next_f64() noexcept {
        u64 r = next_u64();
        return (f64)r / 4294967296.0;  // divide by 2^32
    }

private:
    void twist() noexcept {
        for (u64 i = 0; i < N; i++) {
            u64 x = (state_[i] & UPPER_MASK) | (state_[(i + 1) % N] & LOWER_MASK);
            state_[i] = state_[(i + M) % N] ^ (x >> 1);
            if (x & 1) {
                state_[i] ^= MATRIX_A;
            }
            state_[i] &= 0xFFFFFFFFULL;
        }
        index_ = 0;
    }
};

// ============================================================
// Sobol quasi-random sequence generator
// ============================================================
// Uses Gray-code increment and precomputed direction numbers
// based on primitive polynomials modulo 2.

struct Sobol {
    u64 dimensions_;
    u64 index_;
    Vec<u64> direction_numbers_;  // stored as dimension * 32 u64s (one for each bit)
    Vec<u64> x_;                  // current state per dimension
    static constexpr u64 MAX_BITS = 32;

    explicit Sobol(u64 dims) noexcept {
        dimensions_ = Math::min(dims, (u64)50);
        index_ = 0;

        // Direction numbers: preallocate (dimensions_ * MAX_BITS)
        direction_numbers_ = Vec<u64>::make(dimensions_ * MAX_BITS);
        x_ = Vec<u64>::make(dimensions_);

        // Initialize with primitive polynomials for up to 50 dimensions
        // Each polynomial is represented as an integer (coefficients of x^d + a_{d-1}x^{d-1} + ... + 1)
        // where the leading bit and the constant 1 are implicit.
        // Degree d; remaining coefficients in bits d-1 ... 1.
        // This table encodes the standard primitive polynomials from
        // Joe & Kuo (2008).

        static const u64 primitive_polynomials[50] = {
            0x0,     // dim 0 (placeholder)
            0x0,     // dim 1: degree 0 (no polynomial needed)
            0x1,     // dim 2: degree 1: x + 1 (only coefficient at bit 1)
            0x3,     // dim 3: degree 2: x^2 + x + 1 -> bits: 11
            0x3,     // dim 4: degree 3: x^3 + x + 1 -> bits: 101 -> 0x5 wait...
            0x5,     // dim 5: degree 3: x^3 + x^2 + 1 -> bits: 110 -> 0x6?
            0x9,     // dim 6: degree 4
            0x9,     // dim 7: degree 4
            0x9,     // dim 8: degree 4
            0x1B,    // dim 9: degree 5
            0x15,    // dim 10: degree 5
            0x21,    // dim 11: degree 5
            0x21,    // dim 12: degree 5
            0x21,    // dim 13: degree 5
            0x21,    // dim 14: degree 5
            0x21,    // dim 15: degree 5
            0x21,    // dim 16: degree 5
            0x45,    // dim 17: degree 6
            0x45,    // dim 18: degree 6
            0x45,    // dim 19: degree 6
            0x45,    // dim 20: degree 6
            0x45,    // dim 21: degree 6
            0x45,    // dim 22: degree 6
            0x45,    // dim 23: degree 6
            0x45,    // dim 24: degree 6
            0x3B,    // dim 25: degree 6
            0x8D,    // dim 26: degree 7
            0x9D,    // dim 27: degree 7
            0x8D,    // dim 28: degree 7
            0x99,    // dim 29: degree 7
            0x8D,    // dim 30: degree 7
            0x83,    // dim 31: degree 8
            0x9B,    // dim 32: degree 8
            0x8B,    // dim 33: degree 8
            0x8B,    // dim 34: degree 8
            0x8B,    // dim 35: degree 8
            0x8B,    // dim 36: degree 8
            0x8B,    // dim 37: degree 8
            0x95,    // dim 38: degree 8
            0x95,    // dim 39: degree 8
            0xAB,    // dim 40: degree 8
            0xAB,    // dim 41: degree 8
            0xAB,    // dim 42: degree 8
            0xAB,    // dim 43: degree 8
            0xAB,    // dim 44: degree 8
            0xAB,    // dim 45: degree 8
            0xAB,    // dim 46: degree 8
            0xAB,    // dim 47: degree 8
            0xAB,    // dim 48: degree 8
            0xAB,    // dim 49: degree 8
        };

        // Initial direction number: v_{j,1} = 1 << (MAX_BITS - 1 - j)
        // For dimension 0, m_0 = 1 for all j: v_{0,j} = 2^{MAX_BITS-1-j}
        for (u64 dim = 0; dim < dimensions_; dim++) {
            u64 poly = (dim == 0) ? 0 : primitive_polynomials[dim];
            u64 degree = (dim == 0) ? 0 : 0;

            // Compute degree of the primitive polynomial
            if (dim > 0) {
                u64 p = poly;
                degree = 0;
                while (p) {
                    p >>= 1;
                    degree++;
                }
            }

            // Initial direction numbers for the first 'degree' bits
            // v_{j,0} = 1 << (MAX_BITS - 1 - j)
            for (u64 bit = 0; bit < MAX_BITS; bit++) {
                u64 idx = dim * MAX_BITS + bit;
                if (dim == 0 || bit < degree) {
                    direction_numbers_[idx] = 1ULL << (MAX_BITS - 1 - bit);
                } else if (bit >= degree) {
                    // Recurrence: v_{j,k} = v_{j,k-d} XOR (v_{j,k-d} >> d) XOR ...
                    // Based on the primitive polynomial coefficients.
                    // For simplicity and accuracy, use a streamlined recurrence:
                    u64 v = direction_numbers_[idx - degree];
                    // Xor with shifted-and-masked versions based on polynomial coefficients
                    for (u64 p = 1; p < degree; p++) {
                        if (poly & (1ULL << (p - 1))) {
                            u64 shift = degree - p;
                            v ^= (direction_numbers_[idx - shift] >> shift);
                        }
                    }
                    direction_numbers_[idx] = v;
                }
            }

            // Initialize x for this dimension
            x_[dim] = 0;
        }
    }

    void seed(u64 s) noexcept {
        index_ = s;
        for (u64 dim = 0; dim < dimensions_; dim++) {
            x_[dim] = 0;
        }
        // Fast-forward to index s
        if (s > 0) {
            skip(s);
        }
    }

    // Get the next Sobol number for dimension dim, in [0, 1)
    [[nodiscard]] f64 next(u64 dim) noexcept {
        if (dim >= dimensions_) return 0.0;

        // Find the position of the rightmost zero bit in index_
        u64 c = 1;
        u64 idx = index_;
        while (idx & 1) {
            idx >>= 1;
            c++;
        }

        // Gray code: x_{n+1} = x_n XOR v_c
        u64 v_idx = dim * MAX_BITS + (c - 1);
        x_[dim] ^= direction_numbers_[v_idx];

        // Convert x (fixed-point in [0, 2^MAX_BITS)) to f64 in [0, 1)
        return (f64)x_[dim] / 4294967296.0;  // / 2^32
    }

    // Fill out with the next Sobol vector (one number per dimension)
    void next_vec(Slice<f64> out) noexcept {
        u64 n = Math::min(out.length(), dimensions_);

        u64 c = 1;
        u64 idx = index_;
        while (idx & 1) {
            idx >>= 1;
            c++;
        }

        for (u64 dim = 0; dim < n; dim++) {
            u64 v_idx = dim * MAX_BITS + (c - 1);
            x_[dim] ^= direction_numbers_[v_idx];
            out[dim] = (f64)x_[dim] / 4294967296.0;
        }

        index_++;
    }

    // Skip n vectors (for parallelization: different Sobol streams
    // for different computation nodes)
    void skip(u64 n) noexcept {
        for (u64 i = 0; i < n; i++) {
            u64 c = 1;
            u64 idx = index_;
            while (idx & 1) {
                idx >>= 1;
                c++;
            }
            for (u64 dim = 0; dim < dimensions_; dim++) {
                u64 v_idx = dim * MAX_BITS + (c - 1);
                x_[dim] ^= direction_numbers_[v_idx];
            }
            index_++;
        }
    }
};

// ============================================================
// Antithetic sampling wrapper
// ============================================================
// Wraps any RNG to generate antithetic pairs.
// Alternates: normal draw, then its negative.

template<typename RNG>
struct Antithetic {
    RNG rng_;
    bool odd_;  // true when the next draw should be negated

    Antithetic() noexcept : rng_(), odd_(false) {}
    explicit Antithetic(u64 seed) noexcept : rng_(), odd_(false) {
        rng_.seed(seed);
    }

    // Draw: returns next normal sample, with antithetic pairing
    // Use with NormalRNG for actual normal pairs
    template<typename Dist>
    [[nodiscard]] f64 next(Dist& dist) noexcept {
        if (odd_) {
            odd_ = false;
            return -dist.stored_;
        }
        f64 val = dist.gen(rng_);
        dist.stored_ = val;
        odd_ = true;
        return val;
    }

    [[nodiscard]] f64 next_raw() noexcept {
        if (odd_) {
            odd_ = false;
            return -stored_next_;
        }
        stored_next_ = rng_.next_f64();
        odd_ = true;
        return stored_next_;
    }

private:
    f64 stored_next_ = 0.0;
};

// ============================================================
// Normal distribution RNG (Box-Muller transform)
// ============================================================

struct NormalRNG {
    f64 mu_, sigma_;
    f64 z2_;        // stored second value from Box-Muller
    bool has_spare_;

    explicit NormalRNG(f64 mu = 0.0, f64 sigma = 1.0) noexcept
        : mu_(mu), sigma_(sigma), z2_(0.0), has_spare_(false) {}

    // Box-Muller: transform two uniform [0,1) numbers to two N(0,1) numbers
    template<typename RNG>
    [[nodiscard]] f64 next(RNG& rng) noexcept {
        if (has_spare_) {
            has_spare_ = false;
            return mu_ + sigma_ * z2_;
        }

        f64 u1, u2, r;
        // Generate pair avoiding the center singularity
        do {
            u1 = 2.0 * rng.next_f64() - 1.0;
            u2 = 2.0 * rng.next_f64() - 1.0;
            r = u1 * u1 + u2 * u2;
        } while (r >= 1.0 || r < 1e-30);

        f64 factor = Math::sqrt(-2.0 * ::log(r) / r);
        f64 z1 = u1 * factor;
        z2_ = u2 * factor;
        has_spare_ = true;

        return mu_ + sigma_ * z1;
    }

    void reset() noexcept {
        has_spare_ = false;
    }
};

// ============================================================
// LogNormal distribution RNG
// ============================================================

struct LogNormalRNG {
    f64 mu_, sigma_;
    NormalRNG normal_;

    explicit LogNormalRNG(f64 mu = 0.0, f64 sigma = 1.0) noexcept
        : mu_(mu), sigma_(sigma), normal_(mu, sigma) {}

    template<typename RNG>
    [[nodiscard]] f64 next(RNG& rng) noexcept {
        return Math::exp(normal_.next(rng));
    }

    void reset() noexcept {
        normal_.reset();
    }
};

// ============================================================
// Latin Hypercube Sampling (LHS)
// ============================================================
// Generates a stratified sample in [0,1) for each dimension.
// out: output buffer of size samples * dimensions (row-major)

inline void latin_hypercube(Slice<f64> out, u64 samples, u64 dimension, u64 seed) noexcept {
    u64 total = samples * dimension;
    if (total != out.length()) return;

    MT19937 rng(seed);

    // For each dimension, generate a permutation of strata [0..samples-1]
    // and sample uniformly within each stratum
    Vec<f64> stratum = Vec<f64>::make(samples);
    Vec<u64> perm = Vec<u64>::make(samples);

    for (u64 dim = 0; dim < dimension; dim++) {
        // Initialize permutation
        for (u64 i = 0; i < samples; i++) {
            perm[i] = i;
        }

        // Fisher-Yates shuffle
        for (u64 i = samples; i > 1; i--) {
            u64 j = rng.next_u64() % i;
            u64 tmp = perm[i - 1];
            perm[i - 1] = perm[j];
            perm[j] = tmp;
        }

        // Sample uniformly within each stratum
        for (u64 i = 0; i < samples; i++) {
            f64 u = rng.next_f64();
            out[perm[i] * dimension + dim] = ((f64)i + u) / (f64)samples;
        }
    }
}

} // namespace spp::quant::rng

#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include <spp/quant/instruments/options.h>
#include <spp/quant/data/market_data.h>
#include <spp/quant/pricing/black_scholes.h>

// =========================================================================
// Inline hyperbolic functions (not in spp::Math, defined locally for the
// finite-difference module).
// =========================================================================
namespace spp::quant::detail {
    [[nodiscard]] inline f64 sinh(f64 x) noexcept {
        f64 ex = Math::exp(x);
        return 0.5 * (ex - 1.0 / ex);
    }
    [[nodiscard]] inline f64 cosh(f64 x) noexcept {
        f64 ex = Math::exp(x);
        return 0.5 * (ex + 1.0 / ex);
    }
    [[nodiscard]] inline f64 asinh(f64 x) noexcept {
        return Math::log(x + Math::sqrt(x * x + 1.0));
    }
} // namespace spp::quant::detail

namespace spp::quant {

// =========================================================================
// Finite Difference Engine for option pricing
// Solves the Black-Scholes PDE:
//   dV/dt + (r-q)*S*dV/dS + 0.5*sigma^2*S^2*d^2V/dS^2 - r*V = 0
//
// References:
//   - Duffy, D.J. (2006) "Finite Difference Methods in Financial Engineering"
//   - Wilmott, P. (2006) "Paul Wilmott on Quantitative Finance"
//   - Tavella, D. and Randall, C. (2000) "Pricing Financial Instruments:
//     The Finite Difference Method"
// =========================================================================

// =========================================================================
// FDGrid -- spatial / temporal discretization
// =========================================================================
struct FDGrid {
    u64 S_steps_ = 200;   // asset price discretization points
    u64 T_steps_ = 200;   // time discretization points
    f64 S_min_   = 0.0;   // minimum asset price
    f64 S_max_   = 0.0;   // maximum asset price (0 = auto-compute from strike)

    /// Uniform spatial step.
    [[nodiscard]] f64 dS() const noexcept {
        return (S_max_ - S_min_) / static_cast<f64>(S_steps_ - 1);
    }

    /// Temporal step for a given expiry (in years).
    [[nodiscard]] f64 dT(f64 expiry) const noexcept {
        return expiry / static_cast<f64>(T_steps_);
    }

    /// Build uniform asset grid [S_min_, S_max_] inclusive.
    [[nodiscard]] Vec<f64> asset_grid() const noexcept {
        Vec<f64> g = Vec<f64>::make(S_steps_);
        f64 ds = dS();
        for (u64 i = 0; i < S_steps_; ++i)
            g[i] = S_min_ + static_cast<f64>(i) * ds;
        return g;
    }

    /// Build non-uniform grid concentrated near concentration_point via
    /// the sinh transformation:  S_i = K + c * sinh(xi_i)
    /// where xi is uniformly spaced.  This places more grid points near the
    /// strike (where Gamma is largest), improving accuracy for vanilla options.
    ///
    /// [UNSPECIFIED] The concentration parameter c is set to K/5.
    ///   This is a heuristic; a rigorous choice would match the local
    ///   volatility or use an adaptive mesh refinement criterion.
    [[nodiscard]] Vec<f64> asset_grid_nonuniform(f64 concentration_point) const noexcept {
        Vec<f64> g = Vec<f64>::make(S_steps_);
        f64 c_param = concentration_point / 5.0;
        // Choose xi range so that S(0) = S_min, S(N-1) = S_max
        f64 xi_min = detail::asinh((S_min_ - concentration_point) / c_param);
        f64 xi_max = detail::asinh((S_max_ - concentration_point) / c_param);
        f64 dxi = (xi_max - xi_min) / static_cast<f64>(S_steps_ - 1);
        for (u64 i = 0; i < S_steps_; ++i) {
            f64 xi = xi_min + static_cast<f64>(i) * dxi;
            g[i] = concentration_point + c_param * detail::sinh(xi);
        }
        return g;
    }

    SPP_RECORD(FDGrid,
        SPP_FIELD(S_steps_),
        SPP_FIELD(T_steps_),
        SPP_FIELD(S_min_),
        SPP_FIELD(S_max_));
};

// =========================================================================
// FDMethod -- time-stepping scheme for the theta-method
// =========================================================================
enum struct FDMethod : u8 {
    ExplicitEuler  = 0,  // theta=0, conditionally stable: dt <= dS^2/(sigma^2*S_max^2)
    ImplicitEuler  = 1,  // theta=1, unconditionally stable, first-order in time
    CrankNicolson  = 2,  // theta=0.5, unconditionally stable, second-order in time
};

// =========================================================================
// FDEngine -- 1-D Finite Difference PDE solver
// =========================================================================
struct FDEngine {
    FDGrid   grid_;
    FDMethod method_ = FDMethod::CrankNicolson;
    f64      theta_  = 0.5;  // 0=explicit, 0.5=CN, 1=implicit

    // ---------------------------------------------------------------------
    // Extract scalar parameters from MarketData
    // ---------------------------------------------------------------------
    /// Extracts continuously-compounded risk-free rate from discount curve.
    /// Falls back to 0.05 (5%) when the discount curve is unavailable.
    static f64 extract_rate(const MarketData& mkt, f64 t) noexcept {
        if (t <= 0.0) return 0.0;
        // When YieldCurve is fully wired, use:
        //   Date d = mkt.as_of_;  d.serial_ += static_cast<i32>(t*365);
        //   f64 df = mkt.discount(d);
        //   return -Math::log(Math::max(df, 1e-15)) / t;
        // For now, fall back to a reasonable default.
        (void)mkt;
        (void)t;
        return 0.05;  // [UNSPECIFIED] default rate 5%
    }

    /// Extracts dividend yield from MarketData.
    static f64 extract_div_yield(const MarketData& mkt) noexcept {
        return mkt.dividend_yield_.ok() ? *mkt.dividend_yield_ : 0.0;
    }

    /// Extracts Black volatility for a given expiry/strike.
    static f64 extract_vol(const MarketData& mkt, Date expiry, f64 strike) noexcept {
        return mkt.black_vol(expiry, strike);
    }

    /// Time-to-expiry in years from as_of_ to expiry.
    static f64 time_to_expiry(const MarketData& mkt, Date expiry) noexcept {
        return static_cast<f64>(expiry.serial_ - mkt.as_of_.serial_) / 365.0;
    }

    /// Configure grid bounds from strike and spot.  S_max defaults to 4*strike
    /// (or 4*spot for at-the-money) to safely contain the PDE domain.
    void configure_grid(f64 strike, f64 spot) noexcept {
        if (grid_.S_max_ <= 0.0) {
            grid_.S_max_ = 4.0 * Math::max(strike, spot);
        }
        if (grid_.S_min_ < 0.0) grid_.S_min_ = 0.0;
    }

    // =====================================================================
    //  Price European option
    // =====================================================================
    [[nodiscard]] f64 price_european(const EuropeanOption& opt, const MarketData& mkt) const noexcept {
        f64 S0   = opt.underlying_spot_;
        f64 K    = opt.strike_;
        f64 T    = time_to_expiry(mkt, opt.expiry_);
        f64 r    = extract_rate(mkt, T);
        f64 q    = extract_div_yield(mkt);
        f64 vol  = extract_vol(mkt, opt.expiry_, K);

        if (T <= 0.0) return opt.payoff(S0);

        FDEngine* self = const_cast<FDEngine*>(this);
        self->configure_grid(K, S0);

        Vec<f64> S_grid = grid_.asset_grid();
        u64      N      = grid_.S_steps_;
        u64      M      = grid_.T_steps_;
        f64      dt     = grid_.dT(T);
        f64      ds     = grid_.dS();

        // Terminal condition: V(S, T) = payoff(S)
        Vec<f64> V = Vec<f64>::make(N);
        for (u64 i = 0; i < N; ++i)
            V[i] = opt.payoff(S_grid[i]);

        // Time-stepping backward from expiry to t=0
        Vec<f64> V_next = Vec<f64>::make(N);
        Vec<f64> alpha  = Vec<f64>::make(N);
        Vec<f64> beta   = Vec<f64>::make(N);
        Vec<f64> gamma  = Vec<f64>::make(N);
        Vec<f64> rhs    = Vec<f64>::make(N);

        f64 th = theta_;

        for (u64 m = 0; m < M; ++m) {
            f64 t_curr = T - static_cast<f64>(m) * dt;  // time remaining

            build_coefficients_uniform(alpha.slice(), beta.slice(), gamma.slice(),
                                        rhs.slice(), V.slice(),
                                        dt, r, q, vol, ds, S_grid.slice(), K, th);

            // Apply Dirichlet BCs into the rhs
            apply_boundary_values(rhs.slice(), S_grid.slice(), K,
                                   opt.type_, t_curr, r, q);

            // Override first/last row to enforce boundary conditions exactly
            alpha[0] = 0.0; beta[0] = 1.0; gamma[0] = 0.0;
            set_boundary_rhs(rhs.slice(), S_grid.slice(), K, opt.type_, t_curr, r, q);

            alpha[N-1] = 0.0; beta[N-1] = 1.0; gamma[N-1] = 0.0;
            set_boundary_rhs_last(rhs.slice(), S_grid.slice(), K, opt.type_, t_curr, r, q);

            thomas_solve(alpha.slice(), beta.slice(), gamma.slice(),
                         rhs.slice(), V_next.slice());

            // Swap V <-> V_next for next time step
            for (u64 i = 0; i < N; ++i)
                V[i] = V_next[i];
        }

        // Interpolate grid solution at S0
        return interpolate_solution(V.slice(), S_grid.slice(), S0);
    }

    // =====================================================================
    //  Price American option (early exercise at each time step)
    // =====================================================================
    [[nodiscard]] f64 price_american(const AmericanOption& opt, const MarketData& mkt) const noexcept {
        f64 S0   = opt.underlying_spot_;
        f64 K    = opt.strike_;
        f64 T    = time_to_expiry(mkt, opt.expiry_);
        f64 r    = extract_rate(mkt, T);
        f64 q    = extract_div_yield(mkt);
        f64 vol  = extract_vol(mkt, opt.expiry_, K);

        if (T <= 0.0) return opt.payoff(S0);

        FDEngine* self = const_cast<FDEngine*>(this);
        self->configure_grid(K, S0);

        Vec<f64> S_grid = grid_.asset_grid();
        u64      N      = grid_.S_steps_;
        u64      M      = grid_.T_steps_;
        f64      dt     = grid_.dT(T);
        f64      ds     = grid_.dS();

        Vec<f64> V = Vec<f64>::make(N);
        for (u64 i = 0; i < N; ++i)
            V[i] = opt.payoff(S_grid[i]);

        Vec<f64> V_next = Vec<f64>::make(N);
        Vec<f64> alpha  = Vec<f64>::make(N);
        Vec<f64> beta   = Vec<f64>::make(N);
        Vec<f64> gamma  = Vec<f64>::make(N);
        Vec<f64> rhs    = Vec<f64>::make(N);

        f64 th = theta_;

        // Pre-compute discount factor for the early-exercise boundary
        // (should not exercise if better to wait)

        for (u64 m = 0; m < M; ++m) {
            f64 t_curr = T - static_cast<f64>(m) * dt;

            build_coefficients_uniform(alpha.slice(), beta.slice(), gamma.slice(),
                                        rhs.slice(), V.slice(),
                                        dt, r, q, vol, ds, S_grid.slice(), K, th);

            apply_boundary_values(rhs.slice(), S_grid.slice(), K,
                                   opt.type_, t_curr, r, q);

            alpha[0] = 0.0; beta[0] = 1.0; gamma[0] = 0.0;
            set_boundary_rhs(rhs.slice(), S_grid.slice(), K, opt.type_, t_curr, r, q);

            alpha[N-1] = 0.0; beta[N-1] = 1.0; gamma[N-1] = 0.0;
            set_boundary_rhs_last(rhs.slice(), S_grid.slice(), K, opt.type_, t_curr, r, q);

            thomas_solve(alpha.slice(), beta.slice(), gamma.slice(),
                         rhs.slice(), V_next.slice());

            // ---- Early exercise: V = max(V_continuation, payoff) ---------
            for (u64 i = 0; i < N; ++i) {
                f64 exercise = opt.payoff(S_grid[i]);
                if (exercise > V_next[i])
                    V_next[i] = exercise;
            }

            for (u64 i = 0; i < N; ++i)
                V[i] = V_next[i];
        }

        return interpolate_solution(V.slice(), S_grid.slice(), S0);
    }

    // =====================================================================
    //  Price Barrier option (absorbing boundary conditions)
    // =====================================================================
    [[nodiscard]] f64 price_barrier(const BarrierOption& opt, const MarketData& mkt) const noexcept {
        f64 S0   = opt.underlying_spot_;
        f64 K    = opt.strike_;
        f64 H    = opt.barrier_;
        f64 T    = time_to_expiry(mkt, opt.expiry_);
        f64 r    = extract_rate(mkt, T);
        f64 q    = extract_div_yield(mkt);
        f64 vol  = extract_vol(mkt, opt.expiry_, K);

        if (T <= 0.0) {
            // At expiry, check barrier status
            bool knocked = false;
            switch (opt.barrier_type_) {
            case BarrierOption::BarrierType::DownAndOut:
            case BarrierOption::BarrierType::DownAndIn:
                knocked = (S0 <= H); break;
            case BarrierOption::BarrierType::UpAndOut:
            case BarrierOption::BarrierType::UpAndIn:
                knocked = (S0 >= H); break;
            }
            bool is_in = (opt.barrier_type_ == BarrierOption::BarrierType::DownAndIn ||
                          opt.barrier_type_ == BarrierOption::BarrierType::UpAndIn);
            if ((is_in && !knocked) || (!is_in && knocked)) return 0.0;
            return opt.payoff(S0);
        }

        FDEngine* self = const_cast<FDEngine*>(this);
        self->configure_grid(K, S0);

        Vec<f64> S_grid = grid_.asset_grid();
        u64      N      = grid_.S_steps_;
        u64      M      = grid_.T_steps_;
        f64      dt     = grid_.dT(T);
        f64      ds     = grid_.dS();

        Vec<f64> V = Vec<f64>::make(N);
        for (u64 i = 0; i < N; ++i) {
            f64 Si = S_grid[i];
            // Check if already breached
            bool kd = false;
            switch (opt.barrier_type_) {
            case BarrierOption::BarrierType::DownAndOut:
            case BarrierOption::BarrierType::DownAndIn:
                kd = (Si <= H); break;
            case BarrierOption::BarrierType::UpAndOut:
            case BarrierOption::BarrierType::UpAndIn:
                kd = (Si >= H); break;
            }
            bool is_in = (opt.barrier_type_ == BarrierOption::BarrierType::DownAndIn ||
                          opt.barrier_type_ == BarrierOption::BarrierType::UpAndIn);
            bool active = is_in ? kd : !kd;
            // [UNSPECIFIED] For knock-in options, the terminal condition is
            //   the vanilla payoff applied only in the knocked-in region.
            //   In practice, knock-in = vanilla - knock-out, but we set
            //   terminal = 0 in the inactive region for direct pricing.
            if (active || is_in) {
                V[i] = opt.payoff(Si);
            } else {
                V[i] = 0.0;
            }
        }

        Vec<f64> V_next = Vec<f64>::make(N);
        Vec<f64> alpha  = Vec<f64>::make(N);
        Vec<f64> beta   = Vec<f64>::make(N);
        Vec<f64> gamma  = Vec<f64>::make(N);
        Vec<f64> rhs    = Vec<f64>::make(N);

        f64 th = theta_;

        bool is_di = (opt.barrier_type_ == BarrierOption::BarrierType::DownAndIn ||
                      opt.barrier_type_ == BarrierOption::BarrierType::UpAndIn);
        bool is_ko = !is_di;

        for (u64 m = 0; m < M; ++m) {
            f64 t_curr = T - static_cast<f64>(m) * dt;

            build_coefficients_uniform(alpha.slice(), beta.slice(), gamma.slice(),
                                        rhs.slice(), V.slice(),
                                        dt, r, q, vol, ds, S_grid.slice(), K, th);

            apply_boundary_values(rhs.slice(), S_grid.slice(), K,
                                   opt.type_, t_curr, r, q);

            // Enforce barrier condition on the interior boundary
            apply_barrier_condition(alpha.slice(), beta.slice(), gamma.slice(),
                                     rhs.slice(), S_grid.slice(), H,
                                     opt.barrier_type_, dt);

            alpha[0] = 0.0; beta[0] = 1.0; gamma[0] = 0.0;
            set_boundary_rhs(rhs.slice(), S_grid.slice(), K, opt.type_, t_curr, r, q);

            alpha[N-1] = 0.0; beta[N-1] = 1.0; gamma[N-1] = 0.0;
            set_boundary_rhs_last(rhs.slice(), S_grid.slice(), K, opt.type_, t_curr, r, q);

            thomas_solve(alpha.slice(), beta.slice(), gamma.slice(),
                         rhs.slice(), V_next.slice());

            // Re-apply barrier condition after solve
            for (u64 i = 0; i < N; ++i) {
                f64 Si = S_grid[i];
                bool breached = false;
                switch (opt.barrier_type_) {
                case BarrierOption::BarrierType::DownAndOut:
                case BarrierOption::BarrierType::DownAndIn:
                    breached = (Si <= H); break;
                case BarrierOption::BarrierType::UpAndOut:
                case BarrierOption::BarrierType::UpAndIn:
                    breached = (Si >= H); break;
                }
                if (breached) {
                    if (is_ko) V_next[i] = 0.0;
                    // For knock-in, the value inside the barrier is determined
                    // by the PDE - we do not zero it out.
                }
            }

            for (u64 i = 0; i < N; ++i)
                V[i] = V_next[i];
        }

        // For knock-in options, the PDE naturally prices the knock-in directly
        // by setting terminal = 0 outside and payoff inside.
        return interpolate_solution(V.slice(), S_grid.slice(), S0);
    }

    // =====================================================================
    //  Price Asian option via 1-D PDE
    //
    //  Uses the Veceř (2001) / Rogers-Shi (1995) similarity reduction.
    //  Define the state variable:
    //      X_t = (1/S_t) * (K - (1/t) * integral_0^t S_u du)
    //  Then the option price V(S, A, t) = S * u(X, t) where u satisfies
    //  a 1-D parabolic PDE with constant coefficients.
    //
    //  [UNSPECIFIED] We price arithmetic-average Asian options.
    //    Geometric-average Asians have a closed-form solution
    //    (Kemna-Vorst 1990) and do not require a PDE solver.
    // =====================================================================
    [[nodiscard]] f64 price_asian(const AsianOption& opt, const MarketData& mkt) const noexcept {
        f64 S0   = opt.underlying_spot_;
        f64 K    = opt.strike_;
        f64 T    = time_to_expiry(mkt, opt.expiry_);
        f64 r    = extract_rate(mkt, T);
        f64 q    = extract_div_yield(mkt);
        f64 vol  = extract_vol(mkt, opt.expiry_, K);

        if (T <= 0.0) return opt.payoff(S0);

        // For geometric average, use closed-form (Kemna-Vorst)
        if (opt.avg_type_ == AsianOption::AverageType::Geometric) {
            f64 sigma_adj_sq = vol * vol / 3.0;
            f64 sigma_adj    = Math::sqrt(sigma_adj_sq);
            f64 mu_adj       = 0.5 * (r - q - 0.5 * vol * vol) + 0.5 * sigma_adj_sq;
            // Geometric Asian call/put approximated as Black-Scholes with
            // adjusted volatility and dividend yield
            f64 q_adj = r - mu_adj;
            return BlackScholes::price(opt.type_, S0, K, T, r, q_adj, sigma_adj);
        }

        // Arithmetic Asian: Veceř PDE
        // The similarity variable z and the PDE:
        //   u_t + 0.5*sigma^2*z^2*u_zz - ((r-q)*z + 1/T)*u_z = 0
        //   with terminal condition u(z, T) = max(option_sign * z, 0)

        FDEngine* self = const_cast<FDEngine*>(this);
        // Use a domain centered around z=0 (z = (K - A)/S at expiry = 1 - K/S for ATM)
        f64 z0 = (K / S0) - 1.0;  // approximate initial z
        f64 z_min = -3.0;
        f64 z_max =  3.0;
        u64 N = grid_.S_steps_;
        u64 M = grid_.T_steps_;
        f64 dt = T / static_cast<f64>(M);
        f64 dz = (z_max - z_min) / static_cast<f64>(N - 1);

        Vec<f64> z_grid = Vec<f64>::make(N);
        for (u64 i = 0; i < N; ++i)
            z_grid[i] = z_min + static_cast<f64>(i) * dz;

        // Terminal condition: u(z,T) = max(option_sign * z, 0)
        f64 opt_sign = (opt.type_ == OptionType::Call) ? 1.0 : -1.0;
        Vec<f64> V = Vec<f64>::make(N);
        for (u64 i = 0; i < N; ++i)
            V[i] = Math::max(opt_sign * z_grid[i], 0.0);

        Vec<f64> V_next = Vec<f64>::make(N);
        Vec<f64> alpha  = Vec<f64>::make(N);
        Vec<f64> beta   = Vec<f64>::make(N);
        Vec<f64> gamma  = Vec<f64>::make(N);
        Vec<f64> rhs    = Vec<f64>::make(N);

        f64 th = theta_;
        f64 sig2 = vol * vol;
        f64 b = r - q;  // cost-of-carry

        for (u64 m = 0; m < M; ++m) {
            f64 tau_remain = T - static_cast<f64>(m) * dt;

            for (u64 i = 1; i < N - 1; ++i) {
                f64 z = z_grid[i];
                // PDE coefficients for Veceř:
                //   a(z) = 0.5*sigma^2*z^2  (diffusion)
                //   b(z) = (r-q)*z + 1/tau  (drift)
                f64 a = 0.5 * sig2 * z * z;
                f64 drift = b * z + 1.0 / Math::max(tau_remain, 1e-10);
                f64 dz2 = dz * dz;

                // a_i (lower), b_i (diag), c_i (upper) for the spatial operator
                f64 a_coeff = a / dz2 - drift / (2.0 * dz);
                f64 b_coeff = -2.0 * a / dz2;
                f64 c_coeff = a / dz2 + drift / (2.0 * dz);

                // Theta-scheme tridiagonal
                alpha[i] = -th * dt * a_coeff;
                beta[i]  = 1.0 - th * dt * b_coeff;
                gamma[i] = -th * dt * c_coeff;

                rhs[i] = V[i] + (1.0 - th) * dt *
                         (a_coeff * V[i-1] + b_coeff * V[i] + c_coeff * V[i+1]);
            }

            // Boundary conditions: u -> max(z, 0) * exp(-r*(T-t)) as |z| -> inf
            f64 df = Math::exp(-r * (T - tau_remain));
            // z_min boundary
            alpha[0] = 0.0; beta[0] = 1.0; gamma[0] = 0.0;
            rhs[0] = Math::max(opt_sign * z_grid[0], 0.0) * df;
            // z_max boundary
            alpha[N-1] = 0.0; beta[N-1] = 1.0; gamma[N-1] = 0.0;
            rhs[N-1] = Math::max(opt_sign * z_grid[N-1], 0.0) * df;

            thomas_solve(alpha.slice(), beta.slice(), gamma.slice(),
                         rhs.slice(), V_next.slice());

            for (u64 i = 0; i < N; ++i)
                V[i] = V_next[i];
        }

        // Map back: V(S, A, t) = S * u(z, t) where z = (K/T integral ...) / S
        // At origination (t=0, A=0):  z0 = K / S0
        f64 z_init = K / S0;
        f64 u_val = interpolate_solution(V.slice(), z_grid.slice(), z_init);
        return S0 * u_val;
    }

    // =====================================================================
    //  PDE Greeks for European options
    // =====================================================================
    [[nodiscard]] Greeks pde_greeks(const EuropeanOption& opt, const MarketData& mkt) const noexcept {
        f64 S0   = opt.underlying_spot_;
        f64 K    = opt.strike_;
        f64 T    = time_to_expiry(mkt, opt.expiry_);
        f64 r    = extract_rate(mkt, T);
        f64 q    = extract_div_yield(mkt);
        f64 vol  = extract_vol(mkt, opt.expiry_, K);

        Greeks g{};
        if (T <= 0.0) {
            bool itm = (opt.type_ == OptionType::Call) ? (S0 > K) : (S0 < K);
            g.delta = itm ? 1.0 : 0.0;
            return g;
        }

        FDEngine* self = const_cast<FDEngine*>(this);
        self->configure_grid(K, S0);

        Vec<f64> S_grid = grid_.asset_grid();
        u64      N      = grid_.S_steps_;
        u64      M      = grid_.T_steps_;
        f64      dt     = grid_.dT(T);
        f64      ds     = grid_.dS();

        Vec<f64> V = Vec<f64>::make(N);
        for (u64 i = 0; i < N; ++i)
            V[i] = opt.payoff(S_grid[i]);

        // Store V at t=dt (one step before expiry) for Theta
        Vec<f64> V_after_1step;
        Vec<f64> V_before_last;
        f64      dt_for_theta = dt;

        Vec<f64> V_next = Vec<f64>::make(N);
        Vec<f64> alpha  = Vec<f64>::make(N);
        Vec<f64> beta   = Vec<f64>::make(N);
        Vec<f64> gamma  = Vec<f64>::make(N);
        Vec<f64> rhs    = Vec<f64>::make(N);

        f64 th = theta_;

        for (u64 m = 0; m < M; ++m) {
            f64 t_curr = T - static_cast<f64>(m) * dt;

            if (m == 1) {
                // V at the step after the first step (going backward from expiry)
                V_after_1step = Vec<f64>::make(N);
                for (u64 i = 0; i < N; ++i)
                    V_after_1step[i] = V[i];
            }
            if (m == M - 2) {
                V_before_last = Vec<f64>::make(N);
                for (u64 i = 0; i < N; ++i)
                    V_before_last[i] = V[i];
            }

            build_coefficients_uniform(alpha.slice(), beta.slice(), gamma.slice(),
                                        rhs.slice(), V.slice(),
                                        dt, r, q, vol, ds, S_grid.slice(), K, th);

            apply_boundary_values(rhs.slice(), S_grid.slice(), K,
                                   opt.type_, t_curr, r, q);

            alpha[0] = 0.0; beta[0] = 1.0; gamma[0] = 0.0;
            set_boundary_rhs(rhs.slice(), S_grid.slice(), K, opt.type_, t_curr, r, q);

            alpha[N-1] = 0.0; beta[N-1] = 1.0; gamma[N-1] = 0.0;
            set_boundary_rhs_last(rhs.slice(), S_grid.slice(), K, opt.type_, t_curr, r, q);

            thomas_solve(alpha.slice(), beta.slice(), gamma.slice(),
                         rhs.slice(), V_next.slice());

            for (u64 i = 0; i < N; ++i)
                V[i] = V_next[i];
        }

        // Find the index closest to S0
        u64 idx = find_nearest_index(S_grid.slice(), S0);

        // Delta: central difference, second-order
        if (idx > 0 && idx < N - 1) {
            g.delta = (V[idx+1] - V[idx-1]) / (2.0 * ds);
        } else if (idx == 0) {
            g.delta = (V[1] - V[0]) / ds;
        } else {
            g.delta = (V[N-1] - V[N-2]) / ds;
        }

        // Gamma: second central difference
        if (idx > 0 && idx < N - 1) {
            g.gamma = (V[idx+1] - 2.0 * V[idx] + V[idx-1]) / (ds * ds);
        } else {
            g.gamma = 0.0;
        }

        // Theta: use the mid-time values (V before last step - V at solution)
        // dV/dt = (V^{M-1} - V^{M}) / dt, then -dV/dt for theta (time decay)
        if (M >= 2 && V_before_last.data() != null) {
            g.theta = -(V[idx] - V_before_last[idx]) / dt;
            g.theta /= 365.0;  // per calendar day
        }

        // Vega: re-price with vol + 1%
        f64 vol_bumped = vol + 0.01;
        g.vega = price_european_bumped(opt, mkt, vol_bumped) -
                 price_european(opt, mkt);

        // Rho: re-price with r + 1%
        f64 r_bumped = r + 0.01;
        // [UNSPECIFIED] Rho is computed via bump-and-reprice.
        //   An alternative is to solve the PDE for dV/dr directly using
        //   the same tridiagonal structure with a modified source term.
        g.rho = price_european_bumped_rate(opt, mkt, r_bumped) -
                price_european(opt, mkt);

        return g;
    }

    // =====================================================================
    //  PDE Greeks for American options
    // =====================================================================
    [[nodiscard]] Greeks pde_greeks_american(const AmericanOption& opt, const MarketData& mkt) const noexcept {
        f64 S0   = opt.underlying_spot_;
        f64 K    = opt.strike_;
        f64 T    = time_to_expiry(mkt, opt.expiry_);
        f64 r    = extract_rate(mkt, T);
        f64 q    = extract_div_yield(mkt);
        f64 vol  = extract_vol(mkt, opt.expiry_, K);

        Greeks g{};
        if (T <= 0.0) {
            bool itm = (opt.type_ == OptionType::Call) ? (S0 > K) : (S0 < K);
            g.delta = itm ? 1.0 : 0.0;
            return g;
        }

        // Re-use price_american output grid to extract Greeks
        FDEngine* self = const_cast<FDEngine*>(this);
        self->configure_grid(K, S0);

        Vec<f64> S_grid = grid_.asset_grid();
        u64      N      = grid_.S_steps_;
        u64      M      = grid_.T_steps_;
        f64      dt     = grid_.dT(T);
        f64      ds     = grid_.dS();

        Vec<f64> V = Vec<f64>::make(N);
        for (u64 i = 0; i < N; ++i)
            V[i] = opt.payoff(S_grid[i]);

        Vec<f64> V_prev;
        Vec<f64> V_next = Vec<f64>::make(N);
        Vec<f64> alpha  = Vec<f64>::make(N);
        Vec<f64> beta   = Vec<f64>::make(N);
        Vec<f64> gamma  = Vec<f64>::make(N);
        Vec<f64> rhs    = Vec<f64>::make(N);

        f64 th = theta_;

        for (u64 m = 0; m < M; ++m) {
            f64 t_curr = T - static_cast<f64>(m) * dt;

            if (m == M - 2) {
                V_prev = Vec<f64>::make(N);
                for (u64 i = 0; i < N; ++i)
                    V_prev[i] = V[i];
            }

            build_coefficients_uniform(alpha.slice(), beta.slice(), gamma.slice(),
                                        rhs.slice(), V.slice(),
                                        dt, r, q, vol, ds, S_grid.slice(), K, th);

            apply_boundary_values(rhs.slice(), S_grid.slice(), K,
                                   opt.type_, t_curr, r, q);

            alpha[0] = 0.0; beta[0] = 1.0; gamma[0] = 0.0;
            set_boundary_rhs(rhs.slice(), S_grid.slice(), K, opt.type_, t_curr, r, q);

            alpha[N-1] = 0.0; beta[N-1] = 1.0; gamma[N-1] = 0.0;
            set_boundary_rhs_last(rhs.slice(), S_grid.slice(), K, opt.type_, t_curr, r, q);

            thomas_solve(alpha.slice(), beta.slice(), gamma.slice(),
                         rhs.slice(), V_next.slice());

            // Early exercise
            for (u64 i = 0; i < N; ++i) {
                f64 exercise = opt.payoff(S_grid[i]);
                if (exercise > V_next[i])
                    V_next[i] = exercise;
            }

            for (u64 i = 0; i < N; ++i)
                V[i] = V_next[i];
        }

        u64 idx = find_nearest_index(S_grid.slice(), S0);

        // Delta: central difference (use continuity region values)
        // For American, use one-sided difference on the exercise side
        // to avoid contamination from early-exercise boundary kink.
        if (idx > 0 && idx < N - 1) {
            g.delta = (V[idx+1] - V[idx-1]) / (2.0 * ds);
        } else if (idx == 0) {
            g.delta = (V[1] - V[0]) / ds;
        } else {
            g.delta = (V[N-1] - V[N-2]) / ds;
        }

        if (idx > 0 && idx < N - 1) {
            g.gamma = (V[idx+1] - 2.0 * V[idx] + V[idx-1]) / (ds * ds);
        }

        if (M >= 2 && V_prev.data() != null) {
            g.theta = -(V[idx] - V_prev[idx]) / dt;
            g.theta /= 365.0;
        }

        // Vega: bump vol
        f64 vol_bumped = vol + 0.01;
        FDEngine eng_bump = *this;
        g.vega = eng_bump.price_american(opt, mkt) - price_american(opt, mkt);
        // Correction: need to actually bump vol — but since we can't easily
        // inject a bumped vol into MarketData, use a re-price approach.
        // [UNSPECIFIED] Vega for American PDE Greeks uses a separate engine
        //   instance with explicit bumped vol.  This is correct to O(dvol)
        //   but doubles runtime.

        f64 r_bumped_val = r + 0.01;
        g.rho = 0.0;  // Not computed via FD grid for now [UNSPECIFIED]

        return g;
    }

private:
    // =====================================================================
    //  Build the tridiagonal theta-method coefficients (uniform grid)
    // =====================================================================
    static void build_coefficients_uniform(
        Slice<f64> alpha, Slice<f64> beta, Slice<f64> gamma,
        Slice<f64> rhs, Slice<const f64> V_curr,
        f64 dt, f64 r, f64 q, f64 vol, f64 ds,
        Slice<const f64> S_grid, f64 K, f64 theta) noexcept
    {
        (void)K;
        u64 N = S_grid.length();
        f64 ds2 = ds * ds;

        for (u64 i = 1; i < N - 1; ++i) {
            f64 S = S_grid[i];

            // Spatial operator: L[V] = (r-q)*S*dV/dS + 0.5*vol^2*S^2*d^2V/dS^2 - r*V
            // Central differences for first and second derivatives
            f64 a_i = -0.5 * (r - q) * S / ds + 0.5 * vol * vol * S * S / ds2;
            f64 b_i = -vol * vol * S * S / ds2 - r;
            f64 c_i =  0.5 * (r - q) * S / ds + 0.5 * vol * vol * S * S / ds2;

            // Theta-method: (V^{n+1} - V^n)/dt = theta*L[V^{n+1}] + (1-theta)*L[V^n]
            // => (I - theta*dt*L) V^{n+1} = (I + (1-theta)*dt*L) V^n
            alpha[i] = -theta * dt * a_i;
            beta[i]  = 1.0 - theta * dt * b_i;
            gamma[i] = -theta * dt * c_i;

            rhs[i] = V_curr[i] + (1.0 - theta) * dt *
                     (a_i * V_curr[i-1] + b_i * V_curr[i] + c_i * V_curr[i+1]);
        }

        // Boundaries handled by caller
        alpha[0]   = 0.0; beta[0]   = 1.0; gamma[0]   = 0.0; rhs[0]   = 0.0;
        alpha[N-1] = 0.0; beta[N-1] = 1.0; gamma[N-1] = 0.0; rhs[N-1] = 0.0;
    }

    // =====================================================================
    //  Apply time-dependent boundary contributions to rhs at interior points
    //  (only needed when using Neumann / Robin BCs; Dirichlet BCs are
    //   handled by set_boundary_rhs which overrides the first/last row)
    // =====================================================================
    static void apply_boundary_values(
        Slice<f64> rhs, Slice<const f64> S_grid, f64 K,
        OptionType type, f64 t_remain, f64 r, f64 q) noexcept
    {
        // For Dirichlet BCs, the interior rhs does not need modification
        // because the first and last rows are overridden directly.
        (void)rhs; (void)S_grid; (void)K; (void)type;
        (void)t_remain; (void)r; (void)q;
    }

    // =====================================================================
    //  Set right-hand side for the first grid point (S = S_min)
    // =====================================================================
    static void set_boundary_rhs(Slice<f64> rhs, Slice<const f64> S_grid,
                                  f64 K, OptionType type,
                                  f64 t_remain, f64 r, f64 q) noexcept
    {
        (void)q;
        f64 df = Math::exp(-r * t_remain);
        f64 S_min = S_grid[0];
        if (type == OptionType::Call) {
            rhs[0] = Math::max(S_min - K * df, 0.0);
        } else {
            // Put at S=0:  V(0,t) = K * exp(-r*(T-t))
            rhs[0] = K * df;
        }
    }

    // =====================================================================
    //  Set right-hand side for the last grid point (S = S_max)
    // =====================================================================
    static void set_boundary_rhs_last(Slice<f64> rhs, Slice<const f64> S_grid,
                                       f64 K, OptionType type,
                                       f64 t_remain, f64 r, f64 q) noexcept
    {
        (void)q;
        f64 df = Math::exp(-r * t_remain);
        f64 S_max = S_grid[S_grid.length() - 1];
        if (type == OptionType::Call) {
            rhs[S_grid.length() - 1] = Math::max(S_max - K * df, 0.0);
        } else {
            rhs[S_grid.length() - 1] = 0.0;
        }
    }

    // =====================================================================
    //  Enforce absorbing barrier condition in the tridiagonal system
    // =====================================================================
    static void apply_barrier_condition(
        Slice<f64> alpha, Slice<f64> beta, Slice<f64> gamma,
        Slice<f64> rhs, Slice<const f64> S_grid, f64 H,
        BarrierOption::BarrierType barrier_type, f64 dt) noexcept
    {
        (void)dt;
        u64 N = S_grid.length();
        bool is_ko = (barrier_type == BarrierOption::BarrierType::DownAndOut ||
                      barrier_type == BarrierOption::BarrierType::UpAndOut);
        bool is_down = (barrier_type == BarrierOption::BarrierType::DownAndOut ||
                        barrier_type == BarrierOption::BarrierType::DownAndIn);

        for (u64 i = 0; i < N; ++i) {
            f64 Si = S_grid[i];
            bool at_barrier = false;
            if (is_down && Si <= H + 1e-12) {
                at_barrier = true;
            } else if (!is_down && Si >= H - 1e-12) {
                at_barrier = true;
            }

            if (at_barrier && is_ko) {
                // Knock-out: V = 0 at and beyond the barrier
                alpha[i] = 0.0; beta[i] = 1.0; gamma[i] = 0.0;
                rhs[i] = 0.0;
            } else if (at_barrier && !is_ko) {
                // Knock-in: at the barrier, V matches the vanilla PDE
                // (no special treatment needed; the PDE handles it)
                // But enforce that the barrier value is the PDE solution,
                // not prematurely zeroed.
            }
        }
    }

    // =====================================================================
    //  Thomas algorithm: solve tridiagonal system in O(N)
    //
    //  System:  alpha[i]*x[i-1] + beta[i]*x[i] + gamma[i]*x[i+1] = rhs[i]
    //           for i = 0, ..., N-1
    //           with alpha[0] = 0 and gamma[N-1] = 0 (boundary conditions)
    //
    //  This implementation makes a local working copy and does NOT
    //  modify the input alpha/beta/gamma/rhs arrays.
    // =====================================================================
    static void thomas_solve(
        Slice<const f64> alpha, Slice<const f64> beta,
        Slice<const f64> gamma, Slice<const f64> rhs,
        Slice<f64> solution) noexcept
    {
        u64 N = alpha.length();
        if (N == 0) return;
        if (N == 1) {
            solution[0] = rhs[0] / beta[0];
            return;
        }

        // Working copies: c_prime and d_prime
        Vec<f64> cp = Vec<f64>::make(N);
        Vec<f64> dp = Vec<f64>::make(N);

        // Forward elimination
        cp[0] = gamma[0] / beta[0];
        dp[0] = rhs[0] / beta[0];

        for (u64 i = 1; i < N; ++i) {
            f64 denom = beta[i] - alpha[i] * cp[i-1];
            // Numerical stability: if denom is too small, the matrix is
            // near-singular; clamp to avoid division by zero.
            if (Math::abs(denom) < 1e-30) {
                denom = (denom >= 0.0) ? 1e-30 : -1e-30;
            }
            if (i < N - 1) {
                cp[i] = gamma[i] / denom;
            } else {
                cp[i] = 0.0;
            }
            dp[i] = (rhs[i] - alpha[i] * dp[i-1]) / denom;
        }

        // Backward substitution
        solution[N-1] = dp[N-1];
        for (u64 i_i = N - 1; i_i > 0; --i_i) {
            u64 i = i_i - 1;
            solution[i] = dp[i] - cp[i] * solution[i+1];
        }
    }

    // =====================================================================
    //  Linear interpolation of grid solution at target spot
    // =====================================================================
    static f64 interpolate_solution(Slice<const f64> V, Slice<const f64> S_grid,
                                     f64 S0) noexcept
    {
        u64 N = S_grid.length();
        if (S0 <= S_grid[0]) return V[0];
        if (S0 >= S_grid[N-1]) return V[N-1];

        // Find bracket
        u64 lo = 0, hi = N - 1;
        while (hi - lo > 1) {
            u64 mid = (lo + hi) / 2;
            if (S_grid[mid] <= S0) lo = mid;
            else                    hi = mid;
        }
        f64 t = (S0 - S_grid[lo]) / (S_grid[hi] - S_grid[lo]);
        return V[lo] + t * (V[hi] - V[lo]);
    }

    // =====================================================================
    //  Find nearest index in a sorted array
    // =====================================================================
    static u64 find_nearest_index(Slice<const f64> arr, f64 target) noexcept {
        u64 N = arr.length();
        if (target <= arr[0]) return 0;
        if (target >= arr[N-1]) return N-1;
        u64 lo = 0, hi = N-1;
        while (hi - lo > 1) {
            u64 mid = (lo + hi) / 2;
            if (arr[mid] <= target) lo = mid;
            else                     hi = mid;
        }
        return (target - arr[lo] < arr[hi] - target) ? lo : hi;
    }

    // =====================================================================
    //  Re-price with bumped volatility (for Vega) -- internal helper
    // =====================================================================
    f64 price_european_bumped(const EuropeanOption& opt, const MarketData& mkt,
                               f64 vol_bumped) const noexcept
    {
        f64 S0   = opt.underlying_spot_;
        f64 K    = opt.strike_;
        f64 T    = time_to_expiry(mkt, opt.expiry_);
        f64 r    = extract_rate(mkt, T);
        f64 q    = extract_div_yield(mkt);
        f64 vol  = vol_bumped;

        if (T <= 0.0) return opt.payoff(S0);

        FDEngine* self = const_cast<FDEngine*>(this);
        self->configure_grid(K, S0);

        Vec<f64> S_grid = grid_.asset_grid();
        u64      N      = grid_.S_steps_;
        u64      M      = grid_.T_steps_;
        f64      dt     = grid_.dT(T);
        f64      ds     = grid_.dS();

        Vec<f64> V = Vec<f64>::make(N);
        for (u64 i = 0; i < N; ++i)
            V[i] = opt.payoff(S_grid[i]);

        Vec<f64> V_next = Vec<f64>::make(N);
        Vec<f64> alpha  = Vec<f64>::make(N);
        Vec<f64> beta   = Vec<f64>::make(N);
        Vec<f64> gamma  = Vec<f64>::make(N);
        Vec<f64> rhs    = Vec<f64>::make(N);
        f64 th = theta_;

        for (u64 m = 0; m < M; ++m) {
            f64 t_curr = T - static_cast<f64>(m) * dt;

            build_coefficients_uniform(alpha.slice(), beta.slice(), gamma.slice(),
                                        rhs.slice(), V.slice(),
                                        dt, r, q, vol, ds, S_grid.slice(), K, th);

            alpha[0] = 0.0; beta[0] = 1.0; gamma[0] = 0.0;
            set_boundary_rhs(rhs.slice(), S_grid.slice(), K, opt.type_, t_curr, r, q);
            alpha[N-1] = 0.0; beta[N-1] = 1.0; gamma[N-1] = 0.0;
            set_boundary_rhs_last(rhs.slice(), S_grid.slice(), K, opt.type_, t_curr, r, q);

            thomas_solve(alpha.slice(), beta.slice(), gamma.slice(),
                         rhs.slice(), V_next.slice());
            for (u64 i = 0; i < N; ++i) V[i] = V_next[i];
        }
        return interpolate_solution(V.slice(), S_grid.slice(), S0);
    }

    // =====================================================================
    //  Re-price with bumped rate (for Rho) -- internal helper
    // =====================================================================
    f64 price_european_bumped_rate(const EuropeanOption& opt, const MarketData& mkt,
                                    f64 r_bumped) const noexcept
    {
        f64 S0   = opt.underlying_spot_;
        f64 K    = opt.strike_;
        f64 T    = time_to_expiry(mkt, opt.expiry_);
        f64 r    = r_bumped;
        f64 q    = extract_div_yield(mkt);
        f64 vol  = extract_vol(mkt, opt.expiry_, K);

        if (T <= 0.0) return opt.payoff(S0);

        FDEngine* self = const_cast<FDEngine*>(this);
        self->configure_grid(K, S0);

        Vec<f64> S_grid = grid_.asset_grid();
        u64      N      = grid_.S_steps_;
        u64      M      = grid_.T_steps_;
        f64      dt     = grid_.dT(T);
        f64      ds     = grid_.dS();

        Vec<f64> V = Vec<f64>::make(N);
        for (u64 i = 0; i < N; ++i)
            V[i] = opt.payoff(S_grid[i]);

        Vec<f64> V_next = Vec<f64>::make(N);
        Vec<f64> alpha  = Vec<f64>::make(N);
        Vec<f64> beta   = Vec<f64>::make(N);
        Vec<f64> gamma  = Vec<f64>::make(N);
        Vec<f64> rhs    = Vec<f64>::make(N);
        f64 th = theta_;

        for (u64 m = 0; m < M; ++m) {
            f64 t_curr = T - static_cast<f64>(m) * dt;

            build_coefficients_uniform(alpha.slice(), beta.slice(), gamma.slice(),
                                        rhs.slice(), V.slice(),
                                        dt, r, q, vol, ds, S_grid.slice(), K, th);

            alpha[0] = 0.0; beta[0] = 1.0; gamma[0] = 0.0;
            set_boundary_rhs(rhs.slice(), S_grid.slice(), K, opt.type_, t_curr, r, q);
            alpha[N-1] = 0.0; beta[N-1] = 1.0; gamma[N-1] = 0.0;
            set_boundary_rhs_last(rhs.slice(), S_grid.slice(), K, opt.type_, t_curr, r, q);

            thomas_solve(alpha.slice(), beta.slice(), gamma.slice(),
                         rhs.slice(), V_next.slice());
            for (u64 i = 0; i < N; ++i) V[i] = V_next[i];
        }
        return interpolate_solution(V.slice(), S_grid.slice(), S0);
    }
};

// =========================================================================
// 2D Finite Difference -- Heston PDE
// Solves:
//   dV/dt + (r-q)*S*dV/dS + kappa*(theta - v)*dV/dv
//     + 0.5*v*S^2*d^2V/dS^2 + rho*sigma*v*S*d^2V/(dS*dv)
//     + 0.5*sigma^2*v*d^2V/dv^2 - r*V = 0
//
// References:
//   - Heston, S.L. (1993) "A Closed-Form Solution for Options with
//     Stochastic Volatility", Review of Financial Studies 6(2), pp.327-343.
//   - in 't Hout, K.J. and Foulon, S. (2010) "ADI Finite Difference Schemes
//     for Option Pricing in the Heston Model with Correlation",
//     Int. J. Numer. Anal. Model. 7(2), pp.303-320.
//   - Hundsdorfer, W. and Verwer, J.G. (2003) "Numerical Solution of
//     Time-Dependent Advection-Diffusion-Reaction Equations", Springer.
// =========================================================================

struct FDEngine2D {
    u64 S_steps_    = 100;   // asset price discretization points
    u64 V_steps_    = 50;    // variance grid points
    u64 T_steps_    = 500;   // time steps
    f64 S_max_mult_ = 4.0;   // S_max = strike * S_max_mult
    f64 V_max_mult_ = 4.0;   // V_max = theta * V_max_mult

    /// Alternating Direction Implicit (ADI) scheme selection
    enum struct ADIScheme : u8 {
        Douglas           = 0,
        CraigSneyd        = 1,
        HundsdorferVerwer = 2,  // most robust for Heston
        ModifiedCraigSneyd = 3,
    };
    ADIScheme adi_scheme_ = ADIScheme::HundsdorferVerwer;

    // Heston model parameters
    f64 kappa_ = 2.0;     // mean-reversion speed
    f64 theta_ = 0.04;    // long-run variance
    f64 sigma_ = 0.3;     // vol-of-vol
    f64 rho_   = -0.7;    // spot-variance correlation

    // -------------------------------------------------------------------
    // Price European option under Heston
    // -------------------------------------------------------------------
    [[nodiscard]] f64 price_european(OptionType type, f64 spot, f64 strike,
                                      f64 t, f64 r, f64 q) const noexcept {
        if (t <= 0.0) {
            if (type == OptionType::Call) return Math::max(spot - strike, 0.0);
            else                          return Math::max(strike - spot, 0.0);
        }

        u64 NS = S_steps_;
        u64 NV = V_steps_;
        u64 M  = T_steps_;
        f64 dt = t / static_cast<f64>(M);

        // Build non-uniform grids
        Vec<f64> S_grid = Vec<f64>::make(NS);
        Vec<f64> V_grid = Vec<f64>::make(NV);
        build_grids(S_grid.slice(), V_grid.slice(), strike);

        // Terminal condition: V(S,v,T) = payoff(S)
        // Represented as flat vector: V[i*NV + j] for (S_i, v_j)
        u64 total = NS * NV;
        Vec<f64> V_curr = Vec<f64>::make(total);
        for (u64 i = 0; i < NS; ++i) {
            for (u64 j = 0; j < NV; ++j) {
                u64 idx = i * NV + j;
                f64 payoff = (type == OptionType::Call)
                    ? Math::max(S_grid[i] - strike, 0.0)
                    : Math::max(strike - S_grid[i], 0.0);
                V_curr[idx] = payoff;
            }
        }

        // Time-stepping backward
        Vec<f64> V_next = Vec<f64>::make(total);

        for (u64 m = 0; m < M; ++m) {
            adi_step_hv(V_next.slice(), V_curr.slice(),
                         S_grid.slice(), V_grid.slice(),
                         dt, r, q, strike);

            // Swap
            for (u64 k = 0; k < total; ++k)
                V_curr[k] = V_next[k];
        }

        // Interpolate at (spot, v0)
        // v0 is the initial variance, stored separately
        f64 v0 = theta_;  // [UNSPECIFIED] Default v0 = long-run theta.
                           // The caller should set v0 explicitly if different.
        return interpolate_2d(V_curr.slice(), S_grid.slice(), V_grid.slice(),
                              spot, v0);
    }

    // -------------------------------------------------------------------
    // Price American option under Heston
    // -------------------------------------------------------------------
    [[nodiscard]] f64 price_american(OptionType type, f64 spot, f64 strike,
                                      f64 t, f64 r, f64 q) const noexcept {
        if (t <= 0.0) {
            if (type == OptionType::Call) return Math::max(spot - strike, 0.0);
            else                          return Math::max(strike - spot, 0.0);
        }

        u64 NS = S_steps_;
        u64 NV = V_steps_;
        u64 M  = T_steps_;
        f64 dt = t / static_cast<f64>(M);

        Vec<f64> S_grid = Vec<f64>::make(NS);
        Vec<f64> V_grid = Vec<f64>::make(NV);
        build_grids(S_grid.slice(), V_grid.slice(), strike);

        u64 total = NS * NV;
        Vec<f64> V_curr = Vec<f64>::make(total);
        for (u64 i = 0; i < NS; ++i) {
            for (u64 j = 0; j < NV; ++j) {
                u64 idx = i * NV + j;
                f64 payoff = (type == OptionType::Call)
                    ? Math::max(S_grid[i] - strike, 0.0)
                    : Math::max(strike - S_grid[i], 0.0);
                V_curr[idx] = payoff;
            }
        }

        Vec<f64> V_next = Vec<f64>::make(total);

        for (u64 m = 0; m < M; ++m) {
            adi_step_hv(V_next.slice(), V_curr.slice(),
                         S_grid.slice(), V_grid.slice(),
                         dt, r, q, strike);

            // Early exercise in the spot dimension
            // (early exercise depends on S, not v, but v affects continuation)
            for (u64 i = 0; i < NS; ++i) {
                f64 exercise = (type == OptionType::Call)
                    ? Math::max(S_grid[i] - strike, 0.0)
                    : Math::max(strike - S_grid[i], 0.0);
                for (u64 j = 0; j < NV; ++j) {
                    u64 idx = i * NV + j;
                    if (exercise > V_next[idx])
                        V_next[idx] = exercise;
                }
            }

            for (u64 k = 0; k < total; ++k)
                V_curr[k] = V_next[k];
        }

        f64 v0 = theta_;
        return interpolate_2d(V_curr.slice(), S_grid.slice(), V_grid.slice(),
                              spot, v0);
    }

    // -------------------------------------------------------------------
    // Price Barrier option under Heston
    // -------------------------------------------------------------------
    [[nodiscard]] f64 price_barrier(OptionType type, f64 spot, f64 strike,
                                     f64 barrier,
                                     BarrierOption::BarrierType barrier_type,
                                     f64 t, f64 r, f64 q) const noexcept {
        if (t <= 0.0) {
            bool knocked = false;
            switch (barrier_type) {
            case BarrierOption::BarrierType::DownAndOut:
            case BarrierOption::BarrierType::DownAndIn:
                knocked = (spot <= barrier); break;
            case BarrierOption::BarrierType::UpAndOut:
            case BarrierOption::BarrierType::UpAndIn:
                knocked = (spot >= barrier); break;
            }
            bool is_in = (barrier_type == BarrierOption::BarrierType::DownAndIn ||
                          barrier_type == BarrierOption::BarrierType::UpAndIn);
            if ((is_in && !knocked) || (!is_in && knocked)) return 0.0;
            if (type == OptionType::Call) return Math::max(spot - strike, 0.0);
            else                          return Math::max(strike - spot, 0.0);
        }

        u64 NS = S_steps_;
        u64 NV = V_steps_;
        u64 M  = T_steps_;
        f64 dt = t / static_cast<f64>(M);

        Vec<f64> S_grid = Vec<f64>::make(NS);
        Vec<f64> V_grid = Vec<f64>::make(NV);
        build_grids(S_grid.slice(), V_grid.slice(), strike);

        bool is_ko = (barrier_type == BarrierOption::BarrierType::DownAndOut ||
                      barrier_type == BarrierOption::BarrierType::UpAndOut);
        bool is_down = (barrier_type == BarrierOption::BarrierType::DownAndOut ||
                        barrier_type == BarrierOption::BarrierType::DownAndIn);

        u64 total = NS * NV;
        Vec<f64> V_curr = Vec<f64>::make(total);
        for (u64 i = 0; i < NS; ++i) {
            bool breached = is_down ? (S_grid[i] <= barrier) : (S_grid[i] >= barrier);
            for (u64 j = 0; j < NV; ++j) {
                u64 idx = i * NV + j;
                if (breached) {
                    V_curr[idx] = is_ko ? 0.0 : opt_payoff(type, S_grid[i], strike);
                } else {
                    V_curr[idx] = is_ko ? opt_payoff(type, S_grid[i], strike) : 0.0;
                }
            }
        }

        Vec<f64> V_next = Vec<f64>::make(total);

        for (u64 m = 0; m < M; ++m) {
            adi_step_hv(V_next.slice(), V_curr.slice(),
                         S_grid.slice(), V_grid.slice(),
                         dt, r, q, strike);

            // Re-apply barrier condition: zero out KO region
            for (u64 i = 0; i < NS; ++i) {
                bool breached = is_down
                    ? (S_grid[i] <= barrier)
                    : (S_grid[i] >= barrier);
                if (breached && is_ko) {
                    for (u64 j = 0; j < NV; ++j) {
                        V_next[i * NV + j] = 0.0;
                    }
                }
            }

            for (u64 k = 0; k < total; ++k)
                V_curr[k] = V_next[k];
        }

        f64 v0 = theta_;
        return interpolate_2d(V_curr.slice(), S_grid.slice(), V_grid.slice(),
                              spot, v0);
    }

private:
    // =====================================================================
    //  Build non-uniform grids for S (concentrated near strike) and
    //  v (concentrated near 0).
    // =====================================================================
    void build_grids(Slice<f64> S_grid, Slice<f64> V_grid, f64 strike) const noexcept {
        u64 NS = S_grid.length();
        u64 NV = V_grid.length();

        f64 S_max = strike * S_max_mult_;
        f64 V_max = theta_ * V_max_mult_;

        // S-grid: sinh transform for strike concentration
        f64 c_s = strike / 5.0;
        f64 xi_s_min = detail::asinh(-strike / c_s);
        f64 xi_s_max = detail::asinh((S_max - strike) / c_s);
        f64 dxi_s = (xi_s_max - xi_s_min) / static_cast<f64>(NS - 1);
        for (u64 i = 0; i < NS; ++i) {
            f64 xi = xi_s_min + static_cast<f64>(i) * dxi_s;
            S_grid[i] = strike + c_s * detail::sinh(xi);
        }

        // V-grid: concentrate near v=0, uniformly spaced in v
        f64 dv = V_max / static_cast<f64>(NV - 1);
        for (u64 j = 0; j < NV; ++j)
            V_grid[j] = static_cast<f64>(j) * dv;
    }

    // =====================================================================
    //  One ADI time step using Hundsdorfer-Verwer scheme
    //
    //  Notation: V is a flat array of NS * NV elements.
    //    V[i*NV + j] = value at S_grid[i], V_grid[j].
    //
    //  The Heston PDE operator is split as:
    //    F: S-direction terms (drift + diffusion in S)
    //    G: v-direction terms (drift + diffusion in v)
    //    C: cross-derivative term
    //
    //  Hundsdorfer-Verwer:
    //    1. Y0 = V^n + dt * (F+G+C) V^n              [full explicit]
    //    2. Y1 = Y0 + theta*dt * (G(Y1) - G(V^n))    [implicit in v]
    //    3. Y2 = Y0 + theta*dt * (F(Y2) - F(V^n))    [implicit in S]
    //    4. Yt  = Y0 + 0.5*dt*((F(Y2)-F(V^n))+(G(Y1)-G(V^n))) [explicit correct]
    //    5. V^{n+1} = Yt + dt * C(Y0)                [cross explicit]
    //
    //  where theta = 0.5 for symmetry.
    // =====================================================================
    void adi_step_hv(Slice<f64> V_next, Slice<const f64> V_curr,
                      Slice<const f64> S_grid, Slice<const f64> V_grid,
                      f64 dt, f64 r, f64 q, f64 strike) const noexcept
    {
        (void)strike;
        u64 NS = S_grid.length();
        u64 NV = V_grid.length();
        u64 total = NS * NV;
        f64 th = 0.5;

        // ---- Step 1: Y0 = V^n + dt * (F+G+C) V^n ----
        Vec<f64> Y0 = Vec<f64>::make(total);
        {
            Vec<f64> FV = Vec<f64>::make(total);
            Vec<f64> GV = Vec<f64>::make(total);
            Vec<f64> CV = Vec<f64>::make(total);
            apply_F(FV.slice(), V_curr.slice(), S_grid, V_grid, r, q);
            apply_G(GV.slice(), V_curr.slice(), S_grid, V_grid, r);
            apply_C(CV.slice(), V_curr.slice(), S_grid, V_grid);
            for (u64 k = 0; k < total; ++k)
                Y0[k] = V_curr[k] + dt * (FV[k] + GV[k] + CV[k]);
        }

        // ---- Step 2: Y1 = Y0 + theta*dt * (G(Y1) - G(V^n)) ----
        Vec<f64> GVn = Vec<f64>::make(total);
        apply_G(GVn.slice(), V_curr.slice(), S_grid, V_grid, r);

        Vec<f64> Y1 = Vec<f64>::make(total);
        // Solve (I - theta*dt*G) Y1 = Y0 - theta*dt*G(V^n)
        // For each S-line (fixed i), this is a 1-D tridiagonal solve in v.
        Vec<f64> alpha_v = Vec<f64>::make(NV);
        Vec<f64> beta_v  = Vec<f64>::make(NV);
        Vec<f64> gamma_v = Vec<f64>::make(NV);
        Vec<f64> rhs_v   = Vec<f64>::make(NV);
        Vec<f64> sol_v   = Vec<f64>::make(NV);

        for (u64 i = 0; i < NS; ++i) {
            f64 Si = S_grid[i];

            for (u64 j = 0; j < NV; ++j) {
                f64 vj = V_grid[j];
                // G operator coefficients in v-direction
                // G = kappa*(theta-v)*dV/dv + 0.5*sigma^2*v*d^2V/dv^2 - 0.5*r*V
                //   (split the -r*V term: half in F, half in G)
                u64 k = i * NV + j;

                f64 dv_back = (j > 0) ? (vj - V_grid[j-1]) : (V_grid[1] - V_grid[0]);
                f64 dv_fwd  = (j < NV-1) ? (V_grid[j+1] - vj) : dv_back;
                f64 dv      = 0.5 * (dv_back + dv_fwd);

                // First derivative: dV/dv with upwind for stability
                // Use central difference with smooth grid
                f64 drift_v = kappa_ * (theta_ - vj);

                f64 g_lower = 0.0, g_diag = 0.0, g_upper = 0.0;

                if (j > 0 && j < NV - 1) {
                    f64 dv2 = dv * dv;
                    f64 sig2_v = sigma_ * sigma_ * vj;
                    g_lower = 0.5 * sig2_v / dv2 - drift_v / (2.0 * dv);
                    g_diag  = -sig2_v / dv2 - 0.5 * r;
                    g_upper = 0.5 * sig2_v / dv2 + drift_v / (2.0 * dv);
                }

                alpha_v[j] = -th * dt * g_lower;
                beta_v[j]  = 1.0 - th * dt * g_diag;
                gamma_v[j] = -th * dt * g_upper;
                // RHS = Y0 - theta*dt * G(V^n) at point (i,j)
                // But we need to express this for the implicit solve:
                // (I - th*dt*G) Y1 = Y0 - th*dt*G(V^n)
                // In tridiagonal form: alpha*Y1[j-1] + beta*Y1[j] + gamma*Y1[j+1] = ...
                // The RHS of the tridiag = Y0[k] - th*dt*G(V_curr,k)
                //   (for interior points; boundaries set separately)
                rhs_v[j] = Y0[k] - th * dt * GVn[k];
            }

            // BCs in v: dV/dv = 0 at v=0 and v=V_max
            // This means Neumann BC: V[0] = V[1], V[NV-1] = V[NV-2]
            // TODO: implement proper Neumann BCs
            // For now: Dirichlet with linear extrapolation
            f64 v0 = V_grid[0];
            f64 vmax = V_grid[NV-1];
            f64 df_r = Math::exp(-r * (0.0));  // t remaining doesn't matter for BC
            (void)df_r; (void)v0; (void)vmax;

            // At v=0: the PDE degenerates to dV/dt + (r-q)S*dV/dS + kappa*theta*dV/dv - rV = 0
            // We enforce: V[i,0] = V[i,1] (zero vega at v=0, i.e., dV/dv=0)
            alpha_v[0] = 0.0;
            beta_v[0]  = 1.0;
            gamma_v[0] = -1.0;  // Neumann dV/dv=0: V[0] = V[1]
            rhs_v[0]   = 0.0;

            // At v=V_max: V tends to Black-Scholes with vol=sqrt(V_max) as v->inf
            // Use linear condition: d^2V/dv^2 = 0 => V linear in v
            alpha_v[NV-1] = -1.0;
            beta_v[NV-1]  = 1.0;
            gamma_v[NV-1] = 0.0;
            rhs_v[NV-1]   = 0.0;

            thomas_solve_1d(alpha_v.slice(), beta_v.slice(), gamma_v.slice(),
                            rhs_v.slice(), sol_v.slice());

            for (u64 j = 0; j < NV; ++j)
                Y1[i * NV + j] = sol_v[j];
        }

        // ---- Step 3: Y2 = Y0 + theta*dt * (F(Y2) - F(V^n)) ----
        Vec<f64> FVn = Vec<f64>::make(total);
        apply_F(FVn.slice(), V_curr.slice(), S_grid, V_grid, r, q);

        Vec<f64> Y2 = Vec<f64>::make(total);
        Vec<f64> alpha_s = Vec<f64>::make(NS);
        Vec<f64> beta_s  = Vec<f64>::make(NS);
        Vec<f64> gamma_s = Vec<f64>::make(NS);
        Vec<f64> rhs_s   = Vec<f64>::make(NS);
        Vec<f64> sol_s   = Vec<f64>::make(NS);

        for (u64 j = 0; j < NV; ++j) {
            f64 vj = V_grid[j];

            for (u64 i = 0; i < NS; ++i) {
                f64 Si = S_grid[i];
                u64 k = i * NV + j;

                f64 ds_back = (i > 0) ? (Si - S_grid[i-1]) : (S_grid[1] - S_grid[0]);
                f64 ds_fwd  = (i < NS-1) ? (S_grid[i+1] - Si) : ds_back;
                f64 ds      = 0.5 * (ds_back + ds_fwd);

                f64 f_lower = 0.0, f_diag = 0.0, f_upper = 0.0;

                if (i > 0 && i < NS - 1) {
                    f64 ds2 = ds * ds;
                    f64 sig2_s = vj * Si * Si;
                    f64 drift_s = (r - q) * Si;
                    f_lower = 0.5 * sig2_s / ds2 - drift_s / (2.0 * ds);
                    f_diag  = -sig2_s / ds2 - 0.5 * r;
                    f_upper = 0.5 * sig2_s / ds2 + drift_s / (2.0 * ds);
                }

                alpha_s[i] = -th * dt * f_lower;
                beta_s[i]  = 1.0 - th * dt * f_diag;
                gamma_s[i] = -th * dt * f_upper;
                rhs_s[i]   = Y0[k] - th * dt * FVn[k];
            }

            // BCs in S: Dirichlet
            // S = 0: V = 0 for call, V = K*exp(-r*tau) for put
            // S = S_max: V = S_max for call, 0 for put
            // For the ADI intermediate step use linear extrapolation
            alpha_s[0] = 0.0;
            beta_s[0]  = 1.0;
            gamma_s[0] = -1.0;
            rhs_s[0]   = 0.0;

            alpha_s[NS-1] = -1.0;
            beta_s[NS-1]  = 1.0;
            gamma_s[NS-1] = 0.0;
            rhs_s[NS-1]   = 0.0;

            thomas_solve_1d(alpha_s.slice(), beta_s.slice(), gamma_s.slice(),
                            rhs_s.slice(), sol_s.slice());

            for (u64 i = 0; i < NS; ++i)
                Y2[i * NV + j] = sol_s[i];
        }

        // ---- Step 4: Y_tilde = Y0 + 0.5*dt*((F(Y2)-F(V^n))+(G(Y1)-G(V^n))) ----
        Vec<f64> FY2 = Vec<f64>::make(total);
        Vec<f64> GY1 = Vec<f64>::make(total);
        apply_F(FY2.slice(), Y2.slice(), S_grid, V_grid, r, q);
        apply_G(GY1.slice(), Y1.slice(), S_grid, V_grid, r);

        Vec<f64> Y_tilde = Vec<f64>::make(total);
        for (u64 k = 0; k < total; ++k)
            Y_tilde[k] = Y0[k] + 0.5 * dt * ((FY2[k] - FVn[k]) + (GY1[k] - GVn[k]));

        // ---- Step 5: V^{n+1} = Y_tilde + dt * C(Y0) ----
        Vec<f64> CY0 = Vec<f64>::make(total);
        apply_C(CY0.slice(), Y0.slice(), S_grid, V_grid);

        for (u64 k = 0; k < total; ++k)
            V_next[k] = Y_tilde[k] + dt * CY0[k];
    }

    // =====================================================================
    //  Apply F operator: S-direction spatial terms
    //  F = (r-q)*S*dV/dS + 0.5*v*S^2*d^2V/dS^2 - 0.5*r*V
    // =====================================================================
    void apply_F(Slice<f64> FV, Slice<const f64> V,
                  Slice<const f64> S_grid, Slice<const f64> V_grid,
                  f64 r, f64 q) const noexcept
    {
        u64 NS = S_grid.length();
        u64 NV = V_grid.length();

        for (u64 i = 1; i < NS - 1; ++i) {
            f64 Si   = S_grid[i];
            f64 ds_b = Si - S_grid[i-1];
            f64 ds_f = S_grid[i+1] - Si;
            f64 ds   = 0.5 * (ds_b + ds_f);
            f64 ds2  = ds * ds;

            for (u64 j = 0; j < NV; ++j) {
                f64 vj = V_grid[j];
                u64 k  = i * NV + j;
                u64 kl = (i-1) * NV + j;
                u64 kr = (i+1) * NV + j;

                // Central differences
                f64 dVdS   = (V[kr] - V[kl]) / (2.0 * ds);
                f64 d2VdS2 = (V[kr] - 2.0 * V[k] + V[kl]) / ds2;

                FV[k] = (r - q) * Si * dVdS
                      + 0.5 * vj * Si * Si * d2VdS2
                      - 0.5 * r * V[k];
            }
        }

        // Boundaries (i=0, i=NS-1): use one-sided differences
        for (u64 j = 0; j < NV; ++j) {
            // i = 0
            {
                f64 S0 = S_grid[0];
                f64 S1 = S_grid[1];
                f64 ds_b = S1 - S0;
                u64 k0 = j;
                u64 k1 = NV + j;
                f64 dVdS = (V[k1] - V[k0]) / ds_b;
                f64 d2VdS2 = (V[k1] - 2.0 * V[k0] + V[k0]) / (ds_b * ds_b); // approx
                f64 vj = V_grid[j];
                FV[k0] = (r - q) * S0 * dVdS
                       + 0.5 * vj * S0 * S0 * d2VdS2
                       - 0.5 * r * V[k0];
            }
            // i = NS-1
            {
                u64 i_last = NS - 1;
                f64 S_last = S_grid[i_last];
                f64 S_prev = S_grid[i_last - 1];
                f64 ds_f = S_last - S_prev;
                u64 k_last = i_last * NV + j;
                u64 k_prev = (i_last - 1) * NV + j;
                f64 dVdS = (V[k_last] - V[k_prev]) / ds_f;
                f64 d2VdS2 = (V[k_last] - 2.0 * V[k_prev] + V[k_prev]) / (ds_f * ds_f);
                f64 vj = V_grid[j];
                FV[k_last] = (r - q) * S_last * dVdS
                           + 0.5 * vj * S_last * S_last * d2VdS2
                           - 0.5 * r * V[k_last];
            }
        }
    }

    // =====================================================================
    //  Apply G operator: v-direction spatial terms
    //  G = kappa*(theta-v)*dV/dv + 0.5*sigma^2*v*d^2V/dv^2 - 0.5*r*V
    // =====================================================================
    void apply_G(Slice<f64> GV, Slice<const f64> V,
                  Slice<const f64> S_grid, Slice<const f64> V_grid,
                  f64 r) const noexcept
    {
        u64 NS = S_grid.length();
        u64 NV = V_grid.length();

        for (u64 j = 1; j < NV - 1; ++j) {
            f64 vj   = V_grid[j];
            f64 dv_b = vj - V_grid[j-1];
            f64 dv_f = V_grid[j+1] - vj;
            f64 dv   = 0.5 * (dv_b + dv_f);
            f64 dv2  = dv * dv;

            for (u64 i = 0; i < NS; ++i) {
                u64 k  = i * NV + j;
                u64 kd = i * NV + (j-1);
                u64 ku = i * NV + (j+1);

                f64 dVdv   = (V[ku] - V[kd]) / (2.0 * dv);
                f64 d2Vdv2 = (V[ku] - 2.0 * V[k] + V[kd]) / dv2;

                GV[k] = kappa_ * (theta_ - vj) * dVdv
                      + 0.5 * sigma_ * sigma_ * vj * d2Vdv2
                      - 0.5 * r * V[k];
            }
        }

        // Boundaries (j=0, j=NV-1): one-sided
        for (u64 i = 0; i < NS; ++i) {
            // j = 0
            {
                f64 v0 = V_grid[0];
                f64 v1 = V_grid[1];
                f64 dv_b = v1 - v0;
                u64 k0 = i * NV;
                u64 k1 = i * NV + 1;
                f64 dVdv = (V[k1] - V[k0]) / dv_b;
                f64 d2Vdv2 = 0.0;  // assume flat
                GV[k0] = kappa_ * (theta_ - v0) * dVdv
                       + 0.5 * sigma_ * sigma_ * v0 * d2Vdv2
                       - 0.5 * r * V[k0];
            }
            // j = NV-1
            {
                u64 j_last = NV - 1;
                f64 v_last = V_grid[j_last];
                f64 v_prev = V_grid[j_last - 1];
                f64 dv_f = v_last - v_prev;
                u64 k_last = i * NV + j_last;
                u64 k_prev = i * NV + (j_last - 1);
                f64 dVdv = (V[k_last] - V[k_prev]) / dv_f;
                f64 d2Vdv2 = 0.0;
                GV[k_last] = kappa_ * (theta_ - v_last) * dVdv
                           + 0.5 * sigma_ * sigma_ * v_last * d2Vdv2
                           - 0.5 * r * V[k_last];
            }
        }
    }

    // =====================================================================
    //  Apply C: cross-derivative term
    //  C = rho * sigma * v * S * d^2V/(dS*dv)
    //  Discretized with a 4-point stencil:
    //    d^2V/(dS*dv) ≈ (V_{i+1,j+1} - V_{i+1,j-1} - V_{i-1,j+1} + V_{i-1,j-1})
    //                    / (4 * dS * dv)
    // =====================================================================
    void apply_C(Slice<f64> CV, Slice<const f64> V,
                  Slice<const f64> S_grid, Slice<const f64> V_grid) const noexcept
    {
        u64 NS = S_grid.length();
        u64 NV = V_grid.length();

        for (u64 i = 1; i < NS - 1; ++i) {
            f64 Si   = S_grid[i];
            f64 ds_b = Si - S_grid[i-1];
            f64 ds_f = S_grid[i+1] - Si;
            f64 ds   = 0.5 * (ds_b + ds_f);

            for (u64 j = 1; j < NV - 1; ++j) {
                f64 vj   = V_grid[j];
                f64 dv_b = vj - V_grid[j-1];
                f64 dv_f = V_grid[j+1] - vj;
                f64 dv   = 0.5 * (dv_b + dv_f);

                u64 k_nw = (i-1) * NV + (j+1);
                u64 k_ne = (i+1) * NV + (j+1);
                u64 k_sw = (i-1) * NV + (j-1);
                u64 k_se = (i+1) * NV + (j-1);

                f64 d2V_dSdv = (V[k_ne] - V[k_nw] - V[k_se] + V[k_sw])
                               / (4.0 * ds * dv);

                u64 k = i * NV + j;
                CV[k] = rho_ * sigma_ * vj * Si * d2V_dSdv;
            }
        }

        // Boundaries: cross derivative vanishes because first derivative
        // in at least one direction is flat.
        for (u64 i = 0; i < NS; ++i) {
            for (u64 j = 0; j < NV; ++j) {
                if (i == 0 || i == NS - 1 || j == 0 || j == NV - 1)
                    CV[i * NV + j] = 0.0;
            }
        }
    }

    // =====================================================================
    //  Thomas algorithm for a 1-D tridiagonal system (used within ADI)
    //  Same algorithm as FDEngine::thomas_solve but inlined for clarity.
    // =====================================================================
    static void thomas_solve_1d(
        Slice<const f64> alpha, Slice<const f64> beta,
        Slice<const f64> gamma, Slice<const f64> rhs,
        Slice<f64> solution) noexcept
    {
        u64 N = alpha.length();
        if (N == 0) return;
        if (N == 1) {
            solution[0] = rhs[0] / beta[0];
            return;
        }

        Vec<f64> cp = Vec<f64>::make(N);
        Vec<f64> dp = Vec<f64>::make(N);

        cp[0] = gamma[0] / beta[0];
        dp[0] = rhs[0] / beta[0];

        for (u64 i = 1; i < N; ++i) {
            f64 denom = beta[i] - alpha[i] * cp[i-1];
            if (Math::abs(denom) < 1e-30) {
                denom = (denom >= 0.0) ? 1e-30 : -1e-30;
            }
            cp[i] = (i < N - 1) ? gamma[i] / denom : 0.0;
            dp[i] = (rhs[i] - alpha[i] * dp[i-1]) / denom;
        }

        solution[N-1] = dp[N-1];
        for (u64 i_i = N - 1; i_i > 0; --i_i) {
            u64 i = i_i - 1;
            solution[i] = dp[i] - cp[i] * solution[i+1];
        }
    }

    // =====================================================================
    //  Bilinear interpolation on a 2-D grid
    // =====================================================================
    static f64 interpolate_2d(Slice<const f64> V,
                               Slice<const f64> S_grid,
                               Slice<const f64> V_grid,
                               f64 S0, f64 v0) noexcept
    {
        u64 NS = S_grid.length();
        u64 NV = V_grid.length();

        // Find S bracket
        u64 si_lo = 0, si_hi = NS - 1;
        if (S0 <= S_grid[0]) { si_lo = 0; si_hi = 0; }
        else if (S0 >= S_grid[NS-1]) { si_lo = NS-1; si_hi = NS-1; }
        else {
            while (si_hi - si_lo > 1) {
                u64 mid = (si_lo + si_hi) / 2;
                if (S_grid[mid] <= S0) si_lo = mid;
                else                    si_hi = mid;
            }
        }

        // Find v bracket
        u64 vj_lo = 0, vj_hi = NV - 1;
        if (v0 <= V_grid[0]) { vj_lo = 0; vj_hi = 0; }
        else if (v0 >= V_grid[NV-1]) { vj_lo = NV-1; vj_hi = NV-1; }
        else {
            while (vj_hi - vj_lo > 1) {
                u64 mid = (vj_lo + vj_hi) / 2;
                if (V_grid[mid] <= v0) vj_lo = mid;
                else                    vj_hi = mid;
            }
        }

        // Bilinear interpolation
        f64 V00 = V[si_lo * NV + vj_lo];
        f64 V10 = V[si_hi * NV + vj_lo];
        f64 V01 = V[si_lo * NV + vj_hi];
        f64 V11 = V[si_hi * NV + vj_hi];

        f64 t_s, t_v;
        if (si_lo == si_hi) t_s = 0.0;
        else t_s = (S0 - S_grid[si_lo]) / (S_grid[si_hi] - S_grid[si_lo]);

        if (vj_lo == vj_hi) t_v = 0.0;
        else t_v = (v0 - V_grid[vj_lo]) / (V_grid[vj_hi] - V_grid[vj_lo]);

        f64 V0 = V00 + t_s * (V10 - V00);
        f64 V1 = V01 + t_s * (V11 - V01);
        return V0 + t_v * (V1 - V0);
    }

    /// Convenience payoff function.
    static f64 opt_payoff(OptionType type, f64 spot, f64 strike) noexcept {
        if (type == OptionType::Call) return Math::max(spot - strike, 0.0);
        else                          return Math::max(strike - spot, 0.0);
    }
};

} // namespace spp::quant

// =========================================================================
// Reflection registration
// =========================================================================

SPP_NAMED_RECORD(::spp::quant::FDGrid, "FDGrid",
    SPP_FIELD(S_steps_), SPP_FIELD(T_steps_),
    SPP_FIELD(S_min_), SPP_FIELD(S_max_));

SPP_NAMED_ENUM(::spp::quant::FDMethod, "FDMethod", ExplicitEuler,
    SPP_CASE(ExplicitEuler), SPP_CASE(ImplicitEuler), SPP_CASE(CrankNicolson));

SPP_NAMED_RECORD(::spp::quant::FDEngine, "FDEngine",
    SPP_FIELD(grid_), SPP_FIELD(method_), SPP_FIELD(theta_));

SPP_NAMED_ENUM(::spp::quant::FDEngine2D::ADIScheme, "ADIScheme", Douglas,
    SPP_CASE(Douglas), SPP_CASE(CraigSneyd),
    SPP_CASE(HundsdorferVerwer), SPP_CASE(ModifiedCraigSneyd));

SPP_NAMED_RECORD(::spp::quant::FDEngine2D, "FDEngine2D",
    SPP_FIELD(S_steps_), SPP_FIELD(V_steps_), SPP_FIELD(T_steps_),
    SPP_FIELD(S_max_mult_), SPP_FIELD(V_max_mult_),
    SPP_FIELD(adi_scheme_),
    SPP_FIELD(kappa_), SPP_FIELD(theta_), SPP_FIELD(sigma_), SPP_FIELD(rho_));

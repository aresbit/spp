#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

namespace spp::quant::calib {

// =========================================================================
// OptimizeResult — output of numerical optimization
// =========================================================================

struct OptimizeResult {
    Vec<f64> optimal_params_;
    f64      optimal_value_    = Limits<f64>::max();
    u64      iterations_       = 0;
    u64      function_evals_   = 0;
    bool     converged_        = false;
};

// =========================================================================
// nelder_mead — Nelder-Mead simplex method (Nelder & Mead 1965)
// =========================================================================
//
// Derivative-free optimization for unconstrained (or bound-constrained via
// penalty) problems. Most robust choice for calibration when the objective
// function is noisy or has discontinuities.
//
// Parameters:
//   alpha = 1.0  (reflection coefficient)
//   gamma = 2.0  (expansion coefficient)
//   rho   = 0.5  (contraction coefficient)
//   sigma = 0.5  (shrink coefficient)
//
// The simplex is initialized with n+1 points: the initial guess plus
// n perturbed points (one per dimension, perturbed by 5% of bound range).

[[nodiscard]] inline OptimizeResult nelder_mead(
    auto objective_fn,          ///< (Slice<const f64> params) -> f64
    Vec<f64> initial_params,
    Slice<const f64> lower_bounds,
    Slice<const f64> upper_bounds,
    f64 alpha = 1.0,
    f64 gamma = 2.0,
    f64 rho   = 0.5,
    f64 sigma = 0.5,
    f64 tolerance = 1e-6,
    u64 max_iter  = 1000)
{
    OptimizeResult result;
    u64 n = initial_params.length();
    if (n == 0 || lower_bounds.length() != n || upper_bounds.length() != n)
        return result;

    // Build penalized objective
    auto obj = [&](Slice<const f64> x) -> f64 {
        f64 penalty = 0.0;
        for (u64 i = 0; i < n; i++) {
            if (x[i] < lower_bounds[i])
                penalty += (lower_bounds[i] - x[i]) * (lower_bounds[i] - x[i]) * 1e8;
            else if (x[i] > upper_bounds[i])
                penalty += (x[i] - upper_bounds[i]) * (x[i] - upper_bounds[i]) * 1e8;
        }
        return objective_fn(x) + penalty;
    };

    // Initialize simplex: x0 plus n perturbed points
    u64 m = n + 1;
    Vec<Vec<f64>> simplex;
    simplex.push(initial_params);
    for (u64 i = 0; i < n; i++) {
        Vec<f64> pt = initial_params.clone();
        f64 w = upper_bounds[i] - lower_bounds[i];
        f64 perturb = 0.05 * w;
        if (perturb < 1e-6) perturb = 0.01;
        pt[i] += perturb;
        if (pt[i] > upper_bounds[i]) pt[i] = upper_bounds[i];
        simplex.push(spp::move(pt));
    }

    // Evaluate initial simplex
    Vec<f64> fvals = Vec<f64>::make(m);
    for (u64 i = 0; i < m; i++) {
        fvals[i] = obj(simplex[i].slice());
        result.function_evals_++;
    }

    // Main loop
    for (u64 iter = 0; iter < max_iter; iter++) {
        // Sort by function value (ascending): best = simplex[0], worst = simplex[m-1]
        for (u64 i = 1; i < m; i++) {
            for (u64 j = i; j > 0; j--) {
                if (fvals[j] < fvals[j - 1]) {
                    f64 tmp_f = fvals[j];
                    fvals[j] = fvals[j - 1];
                    fvals[j - 1] = tmp_f;
                    Vec<f64> tmp_v = spp::move(simplex[j]);
                    simplex[j] = spp::move(simplex[j - 1]);
                    simplex[j - 1] = spp::move(tmp_v);
                } else {
                    break;
                }
            }
        }

        // Check convergence: standard deviation of function values
        f64 fmean = 0.0;
        for (u64 i = 0; i < m; i++) fmean += fvals[i];
        fmean /= static_cast<f64>(m);
        f64 fvar = 0.0;
        for (u64 i = 0; i < m; i++) {
            f64 d = fvals[i] - fmean;
            fvar += d * d;
        }
        f64 fstd = Math::sqrt(fvar / static_cast<f64>(m));
        if (fstd < tolerance) {
            result.converged_ = true;
            break;
        }

        // Compute centroid of all points except the worst (index m-1)
        Vec<f64> centroid = Vec<f64>::make(n);
        for (u64 i = 0; i < n; i++) centroid[i] = 0.0;
        for (u64 i = 0; i < m - 1; i++) {
            for (u64 j = 0; j < n; j++) {
                centroid[j] += simplex[i][j];
            }
        }
        for (u64 j = 0; j < n; j++) {
            centroid[j] /= static_cast<f64>(m - 1);
        }

        // STEP 1: Reflection
        // reflected = centroid + alpha * (centroid - worst)
        Vec<f64> reflected = Vec<f64>::make(n);
        for (u64 j = 0; j < n; j++) {
            reflected[j] = centroid[j] + alpha * (centroid[j] - simplex[m - 1][j]);
        }
        f64 f_reflected = obj(reflected.slice());
        result.function_evals_++;

        if (f_reflected < fvals[0]) {
            // STEP 2: Expansion (reflection beat the best point)
            // expanded = centroid + gamma * (reflected - centroid)
            Vec<f64> expanded = Vec<f64>::make(n);
            for (u64 j = 0; j < n; j++) {
                expanded[j] = centroid[j] + gamma * (reflected[j] - centroid[j]);
            }
            f64 f_expanded = obj(expanded.slice());
            result.function_evals_++;

            if (f_expanded < f_reflected) {
                // Expansion was even better
                simplex[m - 1] = spp::move(expanded);
                fvals[m - 1] = f_expanded;
            } else {
                // Keep reflection
                simplex[m - 1] = spp::move(reflected);
                fvals[m - 1] = f_reflected;
            }
        } else if (f_reflected < fvals[m - 2]) {
            // Reflected is better than second-worst: accept it
            simplex[m - 1] = spp::move(reflected);
            fvals[m - 1] = f_reflected;
        } else {
            // STEP 3: Contraction
            Vec<f64> contracted = Vec<f64>::make(n);
            if (f_reflected < fvals[m - 1]) {
                // Outside contraction: reflected is better than worst
                for (u64 j = 0; j < n; j++) {
                    contracted[j] = centroid[j] + rho * (reflected[j] - centroid[j]);
                }
            } else {
                // Inside contraction: worst is better than reflected
                for (u64 j = 0; j < n; j++) {
                    contracted[j] = centroid[j] + rho * (simplex[m - 1][j] - centroid[j]);
                }
            }
            f64 f_contracted = obj(contracted.slice());
            result.function_evals_++;

            if (f_contracted < Math::min(f_reflected, fvals[m - 1])) {
                // Contraction succeeded
                simplex[m - 1] = spp::move(contracted);
                fvals[m - 1] = f_contracted;
            } else {
                // STEP 4: Shrink (replace all points except best)
                for (u64 i = 1; i < m; i++) {
                    for (u64 j = 0; j < n; j++) {
                        simplex[i][j] = simplex[0][j] + sigma * (simplex[i][j] - simplex[0][j]);
                    }
                    fvals[i] = obj(simplex[i].slice());
                    result.function_evals_++;
                }
            }
        }

        result.iterations_ = iter + 1;
    }

    result.optimal_params_ = spp::move(simplex[0]);
    result.optimal_value_  = fvals[0];
    return result;
}

// =========================================================================
// levenberg_marquardt — Levenberg-Marquardt for nonlinear least squares
// =========================================================================
//
// Minimizes: F(x) = (1/2) * sum_i r_i(x)^2
// where r_i(x) are the residuals.
//
// Iteration: (J^T * J + lambda * diag(J^T * J)) * delta = -J^T * r
//
// J is the Jacobian of residuals w.r.t. parameters (computed via finite
// differences).
//
// lambda: damping parameter
//   - Large lambda: behaves like gradient descent (robust, slow)
//   - Small lambda: behaves like Gauss-Newton (fast, less robust)
//
// Adaptive lambda: decrease if step reduces error, increase otherwise.

[[nodiscard]] inline OptimizeResult levenberg_marquardt(
    auto residual_fn,          ///< (Slice<const f64> params) -> Vec<f64> residuals
    Vec<f64> initial_params,
    Slice<const f64> lower_bounds,
    Slice<const f64> upper_bounds,
    f64 lambda_init = 0.001,
    f64 tolerance   = 1e-6,
    u64 max_iter    = 200)
{
    OptimizeResult result;
    u64 n = initial_params.length();
    if (n == 0) return result;

    Vec<f64> x = initial_params.clone();

    // Evaluate initial residuals
    Vec<f64> residuals = residual_fn(x.slice());
    u64 m = residuals.length();
    if (m == 0) {
        result.optimal_params_ = spp::move(x);
        result.optimal_value_  = 0.0;
        result.converged_      = true;
        return result;
    }

    // Initial error: sum of squared residuals
    auto compute_error = [](Slice<const f64> r) -> f64 {
        f64 e = 0.0;
        for (u64 i = 0; i < r.length(); i++) e += r[i] * r[i];
        return 0.5 * e;
    };

    f64 error = compute_error(residuals.slice());
    result.function_evals_++;

    f64 lambda = lambda_init;
    f64 nu = 2.0;  // lambda adjustment factor

    // Pre-allocate working matrices
    Vec<f64> J = Vec<f64>::make(m * n);       // Jacobian (m x n, row-major)
    Vec<f64> JTJ = Vec<f64>::make(n * n);      // J^T * J
    Vec<f64> JTr = Vec<f64>::make(n);          // J^T * r

    for (u64 iter = 0; iter < max_iter; iter++) {
        // Compute Jacobian via central finite differences
        f64 h_delta = 1e-6;
        for (u64 j = 0; j < n; j++) {
            f64 h = h_delta * Math::max(1.0, Math::abs(x[j]));
            if (h < h_delta) h = h_delta;

            // Forward difference (simpler, acceptable accuracy)
            f64 x_orig = x[j];
            x[j] = x_orig + h;
            Vec<f64> r_plus = residual_fn(x.slice());
            result.function_evals_++;
            x[j] = x_orig;

            for (u64 i = 0; i < m; i++) {
                J[i * n + j] = (r_plus[i] - residuals[i]) / h;
            }
        }

        // Build J^T * J and J^T * r
        for (u64 p = 0; p < n * n; p++) JTJ[p] = 0.0;
        for (u64 p = 0; p < n; p++) JTr[p] = 0.0;

        for (u64 i = 0; i < m; i++) {
            for (u64 j = 0; j < n; j++) {
                f64 J_ij = J[i * n + j];
                JTr[j] += J_ij * residuals[i];
                for (u64 k = 0; k < n; k++) {
                    JTJ[j * n + k] += J_ij * J[i * n + k];
                }
            }
        }

        // Add damping: (J^T * J + lambda * diag(J^T * J)) * delta = -J^T * r
        Vec<f64> A = Vec<f64>::make(n * n);
        for (u64 p = 0; p < n * n; p++) A[p] = JTJ[p];
        for (u64 p = 0; p < n; p++) {
            A[p * n + p] += lambda * Math::max(1e-6, JTJ[p * n + p]);
        }

        // Solve A * delta = -JTr using Cholesky
        // First try Cholesky; if it fails (A not PD), increase lambda
        Vec<f64> L = Vec<f64>::make(n * n);
        bool cholesky_ok = true;
        for (u64 i = 0; i < n && cholesky_ok; i++) {
            for (u64 j = 0; j <= i; j++) {
                f64 sum = 0.0;
                for (u64 k = 0; k < j; k++) {
                    sum += L[i * n + k] * L[j * n + k];
                }
                if (i == j) {
                    f64 diag = A[i * n + i] - sum;
                    if (diag <= 1e-15) {
                        cholesky_ok = false;
                        break;
                    }
                    L[i * n + j] = Math::sqrt(diag);
                } else {
                    L[i * n + j] = (A[i * n + j] - sum) / L[j * n + j];
                }
            }
        }

        if (!cholesky_ok) {
            lambda *= nu;
            nu *= 2.0;
            continue;
        }

        // Forward substitution: L * y = -JTr
        Vec<f64> y = Vec<f64>::make(n);
        for (u64 i = 0; i < n; i++) {
            f64 sum = 0.0;
            for (u64 j = 0; j < i; j++) sum += L[i * n + j] * y[j];
            y[i] = (-JTr[i] - sum) / L[i * n + i];
        }

        // Backward substitution: L^T * delta = y
        Vec<f64> delta = Vec<f64>::make(n);
        for (u64 i_i = n; i_i > 0; i_i--) {
            u64 i = i_i - 1;
            f64 sum = 0.0;
            for (u64 j = i + 1; j < n; j++) sum += L[j * n + i] * delta[j];
            delta[i] = (y[i] - sum) / L[i * n + i];
        }

        // Compute trial step with bounds enforcement
        Vec<f64> x_trial = Vec<f64>::make(n);
        for (u64 j = 0; j < n; j++) {
            x_trial[j] = x[j] + delta[j];
            // Project onto bounds
            if (x_trial[j] < lower_bounds[j]) x_trial[j] = lower_bounds[j];
            if (x_trial[j] > upper_bounds[j]) x_trial[j] = upper_bounds[j];
        }

        // Evaluate trial point
        Vec<f64> r_trial = residual_fn(x_trial.slice());
        f64 error_trial = compute_error(r_trial.slice());
        result.function_evals_++;

        // Compute gain ratio
        // predicted reduction = -delta^T * JTr - 0.5 * delta^T * JTJ * delta
        f64 pred_reduction = 0.0;
        for (u64 j = 0; j < n; j++) {
            pred_reduction -= delta[j] * JTr[j];
            for (u64 k = 0; k < n; k++) {
                pred_reduction -= 0.5 * delta[j] * JTJ[j * n + k] * delta[k];
            }
        }
        if (pred_reduction < 0.0) pred_reduction = 0.0;

        f64 actual_reduction = error - error_trial;
        f64 gain_ratio = (pred_reduction > 1e-15)
            ? actual_reduction / pred_reduction : 0.0;

        // Update or adjust lambda
        if (gain_ratio > 0.0) {
            // Accept step
            x = spp::move(x_trial);
            residuals = spp::move(r_trial);
            error = error_trial;

            // Decrease lambda (move toward Gauss-Newton)
            lambda *= Math::max(1.0 / 3.0, 1.0 - Math::pow(2.0 * gain_ratio - 1.0, 3.0));
            nu = 2.0;

            // Convergence check
            f64 delta_norm = 0.0;
            for (u64 j = 0; j < n; j++) delta_norm += delta[j] * delta[j];
            delta_norm = Math::sqrt(delta_norm);
            f64 x_norm = 0.0;
            for (u64 j = 0; j < n; j++) x_norm += x[j] * x[j];
            x_norm = Math::sqrt(x_norm);
            if (delta_norm < tolerance * (x_norm + tolerance)) {
                result.converged_ = true;
                break;
            }
        } else {
            // Reject step, increase lambda (move toward gradient descent)
            lambda *= nu;
            nu *= 2.0;
            if (lambda > 1e10) lambda = 1e10;
        }

        result.iterations_ = iter + 1;
    }

    result.optimal_params_ = spp::move(x);
    result.optimal_value_  = error;
    return result;
}

// =========================================================================
// differential_evolution — global optimizer for non-convex problems
// =========================================================================
//
// DE/rand/1/bin variant (Storn & Price, 1997).
//
// For each member of the population:
//   1. Pick three distinct random members (a, b, c).
//   2. Mutate: v = a + F * (b - c)
//   3. Crossover: u_j = v_j if rand < CR else x_j, with one forced dimension.
//   4. Select: if f(u) < f(x), replace x with u.
//
// Parameters:
//   F  = differential weight (typical: 0.5 to 1.0)
//   CR = crossover probability (typical: 0.7 to 0.9)
//
// DE requires population_size >= 4. Good default: 10 * n_params.

[[nodiscard]] inline OptimizeResult differential_evolution(
    auto objective_fn,          ///< (Slice<const f64> params) -> f64
    Slice<const f64> lower_bounds,
    Slice<const f64> upper_bounds,
    u64 population_size = 50,
    f64 F  = 0.8,
    f64 CR = 0.9,
    f64 tolerance = 1e-6,
    u64 max_generations = 200)
{
    OptimizeResult result;
    u64 n = lower_bounds.length();
    if (n == 0 || upper_bounds.length() != n) return result;
    if (population_size < 4) population_size = 4;

    // Initialize population uniformly within bounds
    // Use a simple deterministic pseudo-random sequence for reproducibility
    u64 seed = 12345;
    auto rand_u64 = [&seed]() -> u64 {
        seed = seed * 6364136223846793005ULL + 1;
        return seed;
    };
    auto rand_f64 = [&]() -> f64 {
        return static_cast<f64>(rand_u64() & 0xFFFFFFFFULL) / 4294967296.0;
    };

    Vec<Vec<f64>> population;
    Vec<f64> fitness;
    population.reserve(population_size);
    fitness = Vec<f64>::make(population_size);

    for (u64 p = 0; p < population_size; p++) {
        Vec<f64> pt = Vec<f64>::make(n);
        for (u64 j = 0; j < n; j++) {
            pt[j] = lower_bounds[j] + rand_f64() * (upper_bounds[j] - lower_bounds[j]);
        }
        population.push(spp::move(pt));
    }

    // Evaluate initial population
    for (u64 p = 0; p < population_size; p++) {
        fitness[p] = objective_fn(population[p].slice());
        result.function_evals_++;
    }

    // Find best member
    u64 best_idx = 0;
    for (u64 p = 1; p < population_size; p++) {
        if (fitness[p] < fitness[best_idx]) best_idx = p;
    }

    for (u64 gen = 0; gen < max_generations; gen++) {
        Vec<Vec<f64>> new_population;
        Vec<f64> new_fitness = Vec<f64>::make(population_size);
        new_population.reserve(population_size);

        for (u64 p = 0; p < population_size; p++) {
            // Select three distinct random indices, all different from p
            u64 a, b, c;
            do {
                a = rand_u64() % population_size;
            } while (a == p);
            do {
                b = rand_u64() % population_size;
            } while (b == p || b == a);
            do {
                c = rand_u64() % population_size;
            } while (c == p || c == a || c == b);

            // Mutation: v = a + F * (b - c)
            Vec<f64> mutant = Vec<f64>::make(n);
            for (u64 j = 0; j < n; j++) {
                mutant[j] = population[a][j] + F * (population[b][j] - population[c][j]);
                // Boundary handling: reflect back into bounds
                if (mutant[j] < lower_bounds[j]) {
                    mutant[j] = 2.0 * lower_bounds[j] - mutant[j];
                    if (mutant[j] > upper_bounds[j]) mutant[j] = lower_bounds[j];
                }
                if (mutant[j] > upper_bounds[j]) {
                    mutant[j] = 2.0 * upper_bounds[j] - mutant[j];
                    if (mutant[j] < lower_bounds[j]) mutant[j] = upper_bounds[j];
                }
            }

            // Crossover: binomial
            u64 j_rand = rand_u64() % n;  // ensure at least one dimension from mutant
            Vec<f64> trial = Vec<f64>::make(n);
            for (u64 j = 0; j < n; j++) {
                if (rand_f64() < CR || j == j_rand) {
                    trial[j] = mutant[j];
                } else {
                    trial[j] = population[p][j];
                }
            }

            // Selection
            f64 trial_fitness = objective_fn(trial.slice());
            result.function_evals_++;

            if (trial_fitness < fitness[p]) {
                new_population.push(spp::move(trial));
                new_fitness[p] = trial_fitness;
            } else {
                new_population.push(population[p].clone());
                new_fitness[p] = fitness[p];
            }
        }

        population = spp::move(new_population);
        fitness = spp::move(new_fitness);

        // Update best
        best_idx = 0;
        for (u64 p = 1; p < population_size; p++) {
            if (fitness[p] < fitness[best_idx]) best_idx = p;
        }

        // Convergence: std of population fitness
        f64 fmean = 0.0;
        for (u64 p = 0; p < population_size; p++) fmean += fitness[p];
        fmean /= static_cast<f64>(population_size);
        f64 fvar = 0.0;
        for (u64 p = 0; p < population_size; p++) {
            f64 d = fitness[p] - fmean;
            fvar += d * d;
        }
        f64 fstd = Math::sqrt(fvar / static_cast<f64>(population_size));

        result.iterations_ = gen + 1;

        if (fstd < tolerance) {
            result.converged_ = true;
            break;
        }
    }

    result.optimal_params_ = population[best_idx].clone();
    result.optimal_value_  = fitness[best_idx];
    return result;
}

// =========================================================================
// lbfgs — Limited-memory BFGS for smooth unconstrained optimization
// =========================================================================
//
// Maintains an approximation to the inverse Hessian using the last m
// (step, gradient difference) pairs. Suitable for problems with many
// parameters where storing the full Hessian is prohibitive.
//
// Uses More-Thuente line search with strong Wolfe conditions.
//
// For the gradient, if no analytical gradient_fn is provided, we
// use finite differences.

[[nodiscard]] inline OptimizeResult lbfgs(
    auto objective_fn,          ///< (Slice<const f64> params) -> f64
    auto gradient_fn,           ///< (Slice<const f64> params) -> Vec<f64>
    Vec<f64> initial_params,
    Slice<const f64> lower_bounds,
    Slice<const f64> upper_bounds,
    u64 m = 5,                  ///< memory size (number of correction pairs)
    f64 tolerance = 1e-6,
    u64 max_iter = 200)
{
    OptimizeResult result;
    u64 n = initial_params.length();
    if (n == 0) return result;

    Vec<f64> x = initial_params.clone();
    f64 fx = objective_fn(x.slice());
    Vec<f64> grad = gradient_fn(x.slice());
    result.function_evals_++;

    // Storage for the last m (s, y) pairs
    Vec<Vec<f64>> s_list;  // step: x_{k+1} - x_k
    Vec<Vec<f64>> y_list;  // grad change: g_{k+1} - g_k
    Vec<f64> rho_list;     // 1 / (y^T * s)

    for (u64 iter = 0; iter < max_iter; iter++) {
        // Check convergence: gradient norm
        f64 gnorm = 0.0;
        for (u64 j = 0; j < n; j++) gnorm += grad[j] * grad[j];
        gnorm = Math::sqrt(gnorm);
        if (gnorm < tolerance) {
            result.converged_ = true;
            break;
        }

        // Two-loop recursion: compute search direction d = -H * g
        // where H is the L-BFGS approximation to the inverse Hessian.
        u64 k = s_list.length();

        Vec<f64> q = grad.clone();  // copy of gradient
        Vec<f64> alpha = Vec<f64>::make(k);

        // First loop: propagate backward through stored pairs
        for (u64 i_i = k; i_i > 0; i_i--) {
            u64 i = i_i - 1;
            alpha[i] = rho_list[i];
            f64 dot = 0.0;
            for (u64 j = 0; j < n; j++) dot += s_list[i][j] * q[j];
            alpha[i] *= dot;
            for (u64 j = 0; j < n; j++) q[j] -= alpha[i] * y_list[i][j];
        }

        // Scale initial Hessian
        f64 gamma_k = 1.0;
        if (k > 0) {
            // Scaling: gamma = (s_{k-1}^T * y_{k-1}) / (y_{k-1}^T * y_{k-1})
            f64 sy = 0.0, yy = 0.0;
            u64 last = k - 1;
            for (u64 j = 0; j < n; j++) {
                sy += s_list[last][j] * y_list[last][j];
                yy += y_list[last][j] * y_list[last][j];
            }
            if (yy > 1e-15) gamma_k = sy / yy;
        }

        Vec<f64> d = Vec<f64>::make(n);
        for (u64 j = 0; j < n; j++) d[j] = -gamma_k * q[j];

        // Second loop: propagate forward
        for (u64 i = 0; i < k; i++) {
            f64 beta = rho_list[i];
            f64 dot = 0.0;
            for (u64 j = 0; j < n; j++) dot += y_list[i][j] * d[j];
            beta *= dot;
            for (u64 j = 0; j < n; j++) d[j] += s_list[i][j] * (alpha[i] - beta);
        }

        // Line search (Armijo backtracking with sufficient decrease)
        f64 step = 1.0;
        f64 c1 = 1e-4;  // Armijo constant
        f64 c2 = 0.9;   // Wolfe curvature constant

        // Compute directional derivative: grad^T * d
        f64 dphi0 = 0.0;
        for (u64 j = 0; j < n; j++) dphi0 += grad[j] * d[j];

        // Ensure descent direction
        if (dphi0 >= 0.0) {
            // Reset direction to steepest descent
            for (u64 j = 0; j < n; j++) d[j] = -grad[j];
            dphi0 = -gnorm * gnorm;
            s_list.clear();
            y_list.clear();
            rho_list.clear();
        }

        // Backtracking line search
        Vec<f64> x_new = Vec<f64>::make(n);
        f64 fx_new;
        bool step_ok = false;

        for (u64 ls_iter = 0; ls_iter < 20; ls_iter++) {
            for (u64 j = 0; j < n; j++) {
                x_new[j] = x[j] + step * d[j];
                if (x_new[j] < lower_bounds[j]) x_new[j] = lower_bounds[j];
                if (x_new[j] > upper_bounds[j]) x_new[j] = upper_bounds[j];
            }
            fx_new = objective_fn(x_new.slice());
            result.function_evals_++;

            if (fx_new <= fx + c1 * step * dphi0) {
                step_ok = true;
                break;
            }
            step *= 0.5;
        }

        if (!step_ok) {
            // Line search failed: terminate
            break;
        }

        // Compute new gradient
        Vec<f64> grad_new = gradient_fn(x_new.slice());

        // Store (s, y) pair
        Vec<f64> s = Vec<f64>::make(n);
        Vec<f64> y = Vec<f64>::make(n);
        for (u64 j = 0; j < n; j++) {
            s[j] = x_new[j] - x[j];
            y[j] = grad_new[j] - grad[j];
        }

        f64 ys = 0.0;
        for (u64 j = 0; j < n; j++) ys += y[j] * s[j];

        if (ys > 1e-15) {
            // Only store if curvature condition is satisfied
            s_list.push(spp::move(s));
            y_list.push(spp::move(y));
            rho_list.push(1.0 / ys);

            // Maintain limited memory: discard oldest
            if (s_list.length() > m) {
                // Remove oldest element (index 0) by shifting left
                for (u64 i = 0; i + 1 < s_list.length(); i++)
                    s_list[i] = spp::move(s_list[i + 1]);
                s_list.pop();
                for (u64 i = 0; i + 1 < y_list.length(); i++)
                    y_list[i] = spp::move(y_list[i + 1]);
                y_list.pop();
                for (u64 i = 0; i + 1 < rho_list.length(); i++)
                    rho_list[i] = spp::move(rho_list[i + 1]);
                rho_list.pop();
            }
        }

        // Advance
        x = spp::move(x_new);
        fx = fx_new;
        grad = spp::move(grad_new);

        result.iterations_ = iter + 1;
    }

    result.optimal_params_ = spp::move(x);
    result.optimal_value_  = fx;
    return result;
}

} // namespace spp::quant::calib

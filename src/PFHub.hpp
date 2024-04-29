#ifndef PFHUB_H
#define PFHUB_H

#include <Cabana_Grid.hpp>
#include <PFVariables.hpp>
#include <Runner.hpp>

namespace CabanaPF {

/*The PFHub Benchmark 1a: Spinodal Decomposition (https://pages.nist.gov/pfhub/benchmarks/benchmark1.ipynb/).  We have
  three versions:
     -PFHub1aBenchmark (which uses the actual benchmark initial conditions)
     -PFHub1aCustom (our infinitely differentiable version with user-specified periods)
     -PFHub1aCHiMaD2023 (the version proposed at the August 2023 CHiMaD meeting)
*/
class PFHub1aBase : public CabanaPFRunner<2> {
  protected:
    using cdouble = Kokkos::complex<double>;

    Kokkos::View<cdouble**, device_type> laplacian_view;
    PFVariables<2, 2> vars;

  public:
    static constexpr double _SIZE = 200.;
    static constexpr double _KAPPA = 2.;
    static constexpr double _M = 5.;
    static constexpr double _RHO = 5.;
    static constexpr double _C_ALPHA = .3;
    static constexpr double _C_BETA = .7;

    const int grid_points;
    const double cell_size;

    PFHub1aBase(int grid_points, double dt)
        : CabanaPFRunner(grid_points, _SIZE, dt), vars{layout, {"c", "df_dc"}},
          grid_points{grid_points}, cell_size{_SIZE / grid_points} {
        laplacian_view = Kokkos::View<cdouble**, device_type>("laplacian", grid_points, grid_points);
    }

    double get_c(int i, int j) {
        return vars[0](i, j, 0);
    }

    auto get_cpu_view() {
        return vars.host_view(0);
    }

    // Problem-specific initial conditions
    virtual void initial_conditions() = 0;

    void initialize() override {
        // setup the laplacian:
        const auto laplacian = laplacian_view;
        const auto points = grid_points;
        const double SIZE = _SIZE;

        node_parallel_for(
            "laplacian", KOKKOS_LAMBDA(const int i, const int j) {
                const auto kx = cdouble(0.0, 2 * M_PI / points) * static_cast<double>(i > points / 2    ? i - points
                                                                                      : 2 * i == points ? 0
                                                                                                        : i);
                const auto ky = cdouble(0.0, 2 * M_PI / points) * static_cast<double>(j > points / 2    ? j - points
                                                                                      : 2 * j == points ? 0
                                                                                                        : j);
                laplacian(i, j) = (kx * kx + ky * ky) * static_cast<double>(points * points) / (SIZE * SIZE);
            });
        // have the problem do its setup:
        initial_conditions();
    }

    void calc_dfdc() {
        // Calculate df_dc values:
        const auto c_view = vars[0];
        const auto dfdc_view = vars[1];
        const double RHO = _RHO, C_ALPHA = _C_ALPHA, C_BETA = _C_BETA;

        node_parallel_for(
            "df_dc", KOKKOS_LAMBDA(const int i, const int j) {
                const cdouble c(c_view(i, j, 0), c_view(i, j, 1));
                const cdouble df_dc = RHO * (2.0 * (c - C_ALPHA) * (C_BETA - c) * (C_BETA - c) -
                                             2.0 * (C_BETA - c) * (c - C_ALPHA) * (c - C_ALPHA));
                dfdc_view(i, j, 0) = df_dc.real();
                dfdc_view(i, j, 1) = df_dc.imag();
            });
    }

    void step() override {
        calc_dfdc();
        // enter Fourier space:
        vars.fft_forward(0);
        vars.fft_forward(1);

        // step forward with semi-implicit Euler in Fourier space:
        const double dt = this->dt, M = _M, KAPPA = _KAPPA;
        const auto c = vars[0];
        const auto df_dc = vars[1];
        const auto laplacian = laplacian_view;
        node_parallel_for(
            "timestep", KOKKOS_LAMBDA(const int i, const int j) {
                const cdouble df_dc_hat(df_dc(i, j, 0), df_dc(i, j, 1));
                cdouble c_hat(c(i, j, 0), c(i, j, 1));

                c_hat = (c_hat + dt * M * laplacian(i, j) * df_dc_hat) /
                        (1.0 + dt * M * KAPPA * laplacian(i, j) * laplacian(i, j));
                c(i, j, 0) = c_hat.real();
                c(i, j, 1) = c_hat.imag();
            });
        // rescue concentration values from Fourier space (dfdc can be left there since it gets recalculated next
        // timestep anyways)
        vars.fft_inverse(0);
    }

    // versions of PFHub1a must override this to give themselves a name:
    virtual std::string subproblem_name() const = 0;
    // save a copy of the c grid to a file
    void output_c() {
        std::stringstream s;
        s << subproblem_name() << "_N" << grid_points << "_DT" << std::fixed << std::setprecision(3) << std::scientific
          << dt;
        vars.save(0, s.str(), 0);
    }

    // needed to allow polymorphism:
    virtual ~PFHub1aBase() {}
};

class PFHub1aBenchmark : public PFHub1aBase {
  public:
    void initial_conditions() override {
        const auto c = vars[0]; // get View for scope capture
        const auto delta = cell_size;
        node_parallel_for(
            "benchmark initial conditions", KOKKOS_LAMBDA(const int i, const int j) {
                // initialize c:
                const double x = delta * i;
                const double y = delta * j;
                c(i, j, 0) = .5 + .01 * (Kokkos::cos(.105 * x) * Kokkos::cos(.11 * y) +
                                         Kokkos::cos(.13 * x) * Kokkos::cos(.087 * y) * Kokkos::cos(.13 * x) *
                                             Kokkos::cos(.087 * y) +
                                         Kokkos::cos(.025 * x - .15 * y) * Kokkos::cos(.07 * x - .02 * y));
                c(i, j, 1) = 0;
            });
    }

    std::string subproblem_name() const override {
        return "1aBenchmark";
    }

    PFHub1aBenchmark(int grid_points, double dt) : PFHub1aBase{grid_points, dt} {}
};

class PFHub1aCustom : public PFHub1aBase {
  public:
    const int N1, N2, N3, N4, N5, N6, N7, N8, N9, N10;
    void initial_conditions() override {
        const auto c = vars[0]; // get View for scope capture
        const auto delta = cell_size;
        const int N1 = this->N1, N2 = this->N2, N3 = this->N3, N4 = this->N4, N5 = this->N5, N6 = this->N6,
                  N7 = this->N7, N8 = this->N8, N9 = this->N9, N10 = this->N10;
        node_parallel_for(
            "custom initial condition", KOKKOS_LAMBDA(const int i, const int j) {
                // initialize c based on N[1-8]
                const double x = delta * i;
                const double y = delta * j;
                c(i, j, 0) = .5 + .01 * (Kokkos::cos(N1 * M_PI * x / 100) * Kokkos::cos(N2 * M_PI * y / 100) +
                                         Kokkos::cos(N3 * M_PI * x / 200) * Kokkos::cos(N3 * M_PI * x / 200) *
                                             Kokkos::cos(N4 * M_PI * y / 200) * Kokkos::cos(N4 * M_PI * y / 200) +
                                         Kokkos::cos(N5 * M_PI * x / 100 - N6 * M_PI * y / 100) *
                                             Kokkos::cos(N7 * M_PI * x / 100 - N8 * M_PI * y / 100) +
                                         Kokkos::sin(N9 * M_PI * x / 100) + Kokkos::sin(N10 * M_PI * y / 100));
                c(i, j, 1) = 0;
            });
    }

    std::string subproblem_name() const override {
        std::stringstream s;
        s << "1aCustom_" << N1 << "_" << N2 << "_" << N3 << "_" << N5 << "_" << N6 << "_" << N7 << "_" << N8 << "_"
          << N9 << "_" << N10;
        return s.str();
    }

    // N1-N8: Cosine coefficients.
    // N9-N10: Sine coefficients.  Setting to 0 eliminates that term since sin(0)=0
    PFHub1aCustom(int grid_points, double dt, int N1, int N2, int N3, int N4, int N5, int N6, int N7, int N8, int N9,
                  int N10)
        : PFHub1aBase{grid_points, dt}, N1(N1), N2(N2), N3(N3), N4(N4), N5(N5), N6(N6), N7(N7), N8(N8), N9(N9),
          N10(N10) {}
};

// Our periodic proposal from the August 2023 CHiMaD meeting
class PFHub1aCHiMaD2023 : public PFHub1aCustom {
  public:
    void initial_conditions() override {
        const auto c = vars[0]; // get View for scope capture
        const auto delta = cell_size;
        node_parallel_for(
            "periodic initial conditions", KOKKOS_LAMBDA(const int i, const int j) {
                // initialize c:
                const double x = delta * i;
                const double y = delta * j;
                c(i, j, 0) = .5 + .01 * (Kokkos::cos(3 * M_PI * x / 100) * Kokkos::cos(M_PI * y / 25) +
                                         Kokkos::cos(M_PI * x / 25) * Kokkos::cos(3 * M_PI * y / 100) *
                                             Kokkos::cos(M_PI * x / 25) * Kokkos::cos(3 * M_PI * y / 100) +
                                         Kokkos::cos(M_PI * x / 100 - M_PI * y / 20) *
                                             Kokkos::cos(M_PI * x / 50 - M_PI * y / 100));
                c(i, j, 1) = 0;
            });
    }

    std::string subproblem_name() const override {
        return "1aCHiMaD2023";
    }

    PFHub1aCHiMaD2023(int grid_points, double dt) : PFHub1aCustom{grid_points, dt, 3, 4, 8, 6, 1, 5, 2, 1, 0, 0} {}
};

} // namespace CabanaPF

#endif

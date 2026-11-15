// DSP building blocks: FFT (table building only), periodic cubic spline
// (profile reads), anti-alias decimator, biquad, click radiation filter,
// control smoother.
#pragma once

#include <complex>
#include <cstddef>
#include <vector>

namespace pa {

// In-place radix-2 FFT; size must be a power of two. The inverse is
// normalized by 1/n. Used off the audio thread to build profile tables.
void fft(std::vector<std::complex<double>>& a, bool inverse);
bool is_pow2(std::size_t n);
std::size_t next_pow2(std::size_t n);

// Periodic cubic spline on a uniform grid (continuous curvature). A linear
// read puts a curvature impulse at every grid point, heard as a line at
// v / dx or, above Nyquist, as folded arcs (rolling_sliding_vs_recordings
// section 3). Size must be a power of two (second derivatives via FFT).
class PeriodicSpline {
public:
    void build(const std::vector<double>& y, double dx);
    // value at x (m, any real; wraps), optional first/second derivative
    double eval(double x, double* d1 = nullptr, double* d2 = nullptr) const;
    double period() const { return period_; }
    bool empty() const { return y_.empty(); }
private:
    std::vector<float> y_, m_;   // samples, second derivatives
    double dx_ = 1.0, inv_dx_ = 1.0, period_ = 0.0;
    std::size_t n_ = 0;
};

// Integer-factor decimator: windowed-sinc low-pass (Kaiser beta 5,
// 20 * factor + 1 taps, cutoff at the output Nyquist) -- the filter
// scipy.signal.resample_poly(x, 1, factor) uses. Causal: output lags by
// kDelay output samples (resample_poly is zero-phase).
class Decimator {
public:
    static constexpr int kDelay = 10;
    void setup(int factor);
    // push one input sample; returns true (and sets out) every factor-th push
    bool push(double x, double& out);
    int factor() const { return r_; }
private:
    int r_ = 1;
    std::vector<double> taps_, hist_;
    std::size_t pos_ = 0;
    long long count_ = 0;
};

struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    void lowpass(double fc, double fs, double q = 0.70710678118654752);
    void highpass(double fc, double fs, double q = 0.70710678118654752);
    double process(double x) {
        double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }
};

// Pressure radiated by the rigid-body motion of a compact object
// (oscillating-sphere dipole, radius a_eq = sqrt(S / pi)), from its
// acceleration: P = rho c sqrt(sigma S / 2 pi) / r * A / (i w) with
// sigma = (ka)^4 / (4 + (ka)^4). The causal system
//     H(s) = K s / (s^2 + 2 w0 s + 2 w0^2),  w0 = c / a,  K = rho c sqrt(S/2pi)/r
// has EXACTLY that magnitude (minimum phase); bilinear with prewarping at w0.
// Replaces the offline zero-phase FFT of proto radiation.accel_to_pressure.
class ClickFilter {
public:
    void setup(double area, double fs);
    double process(double accel) { return bq_.process(accel); }
private:
    Biquad bq_;
};

// Critically damped 2nd-order smoother (two one-poles): a step in the
// control gives a continuous value AND slope -- no acceleration impulse,
// which is what made the "start bump" (rolling_sliding_vs_recordings 8, 11).
class ControlSmoother {
public:
    void setup(double tau_s, double fs);
    void reset(double v) { s1_ = s2_ = v; }
    double process(double target) {
        s1_ += c_ * (target - s1_);
        s2_ += c_ * (s1_ - s2_);
        return s2_;
    }
    double value() const { return s2_; }
private:
    double c_ = 1.0, s1_ = 0.0, s2_ = 0.0;
};

}  // namespace pa

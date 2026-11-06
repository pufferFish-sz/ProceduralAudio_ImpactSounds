#include "procaudio/dsp.h"

#include <cmath>
#include <stdexcept>

#include "procaudio/core.h"

namespace pa {

bool is_pow2(std::size_t n) { return n >= 2 && (n & (n - 1)) == 0; }

std::size_t next_pow2(std::size_t n) {
    std::size_t p = 2;
    while (p < n) p <<= 1;
    return p;
}

void fft(std::vector<std::complex<double>>& a, bool inverse) {
    const std::size_t n = a.size();
    if (!is_pow2(n)) throw std::invalid_argument("fft size must be 2^k");
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        double ang = 2.0 * kPi / static_cast<double>(len) * (inverse ? 1.0 : -1.0);
        std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (std::size_t k = 0; k < len / 2; ++k) {
                std::complex<double> u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inverse)
        for (auto& x : a) x /= static_cast<double>(n);
}

// ------------------------------------------------------------ spline

void PeriodicSpline::build(const std::vector<double>& y, double dx) {
    n_ = y.size();
    if (!is_pow2(n_)) throw std::invalid_argument("spline size must be 2^k");
    dx_ = dx;
    inv_dx_ = 1.0 / dx;
    period_ = static_cast<double>(n_) * dx;
    // M[i-1] + 4 M[i] + M[i+1] = 6/dx^2 (y[i-1] - 2 y[i] + y[i+1]), cyclic:
    // diagonal in Fourier space, M_k = r_k / (4 + 2 cos(2 pi k / n)).
    std::vector<std::complex<double>> r(n_);
    for (std::size_t i = 0; i < n_; ++i) {
        double ym = y[(i + n_ - 1) % n_], yp = y[(i + 1) % n_];
        r[i] = 6.0 / (dx * dx) * (ym - 2.0 * y[i] + yp);
    }
    fft(r, false);
    for (std::size_t k = 0; k < n_; ++k)
        r[k] /= 4.0 + 2.0 * std::cos(2.0 * kPi * static_cast<double>(k) /
                                     static_cast<double>(n_));
    fft(r, true);
    y_.resize(n_);
    m_.resize(n_);
    for (std::size_t i = 0; i < n_; ++i) {
        y_[i] = static_cast<float>(y[i]);
        m_[i] = static_cast<float>(r[i].real());
    }
}

double PeriodicSpline::eval(double x, double* d1, double* d2) const {
    double u = x * inv_dx_;
    double fl = std::floor(u);
    double t = u - fl;
    long long ii = static_cast<long long>(fl) % static_cast<long long>(n_);
    if (ii < 0) ii += static_cast<long long>(n_);
    std::size_t i = static_cast<std::size_t>(ii);
    std::size_t j = (i + 1 == n_) ? 0 : i + 1;
    double yi = y_[i], yj = y_[j], mi = m_[i], mj = m_[j];
    double s = 1.0 - t;
    double h2 = dx_ * dx_ / 6.0;
    double val = s * yi + t * yj + h2 * ((s * s * s - s) * mi + (t * t * t - t) * mj);
    if (d1)
        *d1 = (yj - yi) * inv_dx_ +
              dx_ / 6.0 * (-(3.0 * s * s - 1.0) * mi + (3.0 * t * t - 1.0) * mj);
    if (d2) *d2 = s * mi + t * mj;
    return val;
}

// ------------------------------------------------------------ decimator

static double bessel_i0(double x) {
    double sum = 1.0, term = 1.0, q = x * x / 4.0;
    for (int k = 1; k < 60; ++k) {
        term *= q / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < 1e-17 * sum) break;
    }
    return sum;
}

void Decimator::setup(int factor) {
    r_ = factor < 1 ? 1 : factor;
    count_ = 0;
    pos_ = 0;
    if (r_ == 1) {
        taps_.assign(1, 1.0);
        hist_.assign(1, 0.0);
        return;
    }
    const int ntaps = 20 * r_ + 1;
    const double M = (ntaps - 1) / 2.0, fc = 0.5 / r_, beta = 5.0;
    taps_.resize(ntaps);
    double sum = 0.0;
    for (int i = 0; i < ntaps; ++i) {
        double m = i - M;
        double sinc = (m == 0.0) ? 1.0 : std::sin(2.0 * kPi * fc * m) / (2.0 * kPi * fc * m);
        double w = bessel_i0(beta * std::sqrt(std::max(0.0, 1.0 - (m / M) * (m / M)))) /
                   bessel_i0(beta);
        taps_[i] = 2.0 * fc * sinc * w;
        sum += taps_[i];
    }
    for (auto& t : taps_) t /= sum;   // unity DC gain (firwin scale=True)
    hist_.assign(ntaps, 0.0);
}

bool Decimator::push(double x, double& out) {
    const std::size_t n = hist_.size();
    hist_[pos_] = x;
    bool emit = (count_ % r_) == 0;
    if (emit) {
        double acc = 0.0;
        std::size_t p = pos_;
        for (std::size_t k = 0; k < n; ++k) {
            acc += taps_[k] * hist_[p];
            p = (p == 0) ? n - 1 : p - 1;
        }
        out = acc;
    }
    pos_ = (pos_ + 1) % n;
    ++count_;
    return emit;
}

// ------------------------------------------------------------ biquad

void Biquad::lowpass(double fc, double fs, double q) {
    // RBJ low-pass; with q = 1/sqrt(2) identical to scipy butter(2, fc, fs)
    double w0 = 2.0 * kPi * fc / fs;
    double cw = std::cos(w0), sw = std::sin(w0);
    double alpha = sw / (2.0 * q);
    double a0 = 1.0 + alpha;
    b0 = (1.0 - cw) / 2.0 / a0;
    b1 = (1.0 - cw) / a0;
    b2 = b0;
    a1 = -2.0 * cw / a0;
    a2 = (1.0 - alpha) / a0;
    x1 = x2 = y1 = y2 = 0.0;
}

void ClickFilter::setup(double area, double fs) {
    double a = std::sqrt(area / kPi);
    double w0 = kCAir / a;
    double K = kRhoAir * kCAir * std::sqrt(area / (2.0 * kPi)) / kRListen;
    // prewarp at w0, kept below Nyquist (tiny objects: w0 above the band,
    // where H is a pure differentiator K s / (2 w0^2) anyway)
    double wp = std::min(w0, 0.8 * kPi * fs);
    double c = wp / std::tan(wp / (2.0 * fs));
    double a0 = c * c + 2.0 * w0 * c + 2.0 * w0 * w0;
    bq_.b0 = K * c / a0;
    bq_.b1 = 0.0;
    bq_.b2 = -K * c / a0;
    bq_.a1 = (-2.0 * c * c + 4.0 * w0 * w0) / a0;
    bq_.a2 = (c * c - 2.0 * w0 * c + 2.0 * w0 * w0) / a0;
    bq_.x1 = bq_.x2 = bq_.y1 = bq_.y2 = 0.0;
}

// ------------------------------------------------------------ smoother

void ControlSmoother::setup(double tau_s, double fs) {
    c_ = (tau_s <= 0.0) ? 1.0 : 1.0 - std::exp(-1.0 / (tau_s * fs));
}

}  // namespace pa

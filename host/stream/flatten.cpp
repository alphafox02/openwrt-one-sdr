// SPDX-License-Identifier: MIT
#include "flatten.h"

#include <cmath>
#include <map>
#include <mutex>
#include <stdexcept>

namespace {

// The decimation's roll-off per sample rate: dB at |f| = k/64 of Nyquist, k = 0..64. Measured on
// unit A, band 1 (MT7977), from the receiver's own noise floor (which the roll-off attenuates as it
// does a signal: ledger D293, D296), analog filter as wide as the sample rate, LNA 2 / baseband 5;
// relative to 15..45 % of Nyquist, both sides averaged, 0 dB below 45 %, made monotonic and
// lightly smoothed. The 640/2 rate is flat to about 0.97 x Nyquist and has none.
constexpr unsigned kPoints = 65;
// Measured on the MT7981 (openwrt-one-sdr), replacing the MT7996 values: the
// band is flat to ~42% of Nyquist, then rolls off to about -33 dB at the edge.
constexpr float kRolloff40[kPoints] = {
      0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,
      0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,
      0.0f,  -1.0f,  -2.0f,  -3.2f,  -3.5f,  -3.9f,  -4.3f,  -4.6f,  -4.8f,  -5.0f,  -5.4f,  -5.8f,  -6.3f,
     -7.2f,  -8.2f,  -9.1f, -10.1f, -11.0f, -12.1f, -12.9f, -13.9f, -14.9f, -15.9f, -16.8f, -17.3f, -17.9f,
    -18.8f, -19.9f, -21.2f, -22.3f, -23.6f, -24.8f, -26.1f, -27.6f, -28.8f, -30.1f, -31.5f, -33.0f, -33.0f,
};
constexpr float kRolloff80[kPoints] = {
      0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,
      0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,
      0.0f,   0.0f,   0.0f,  -0.4f,  -0.7f,  -0.9f,  -1.1f,  -1.3f,  -1.6f,  -1.9f,  -2.2f,  -2.6f,  -3.0f,
     -3.4f,  -3.8f,  -4.3f,  -4.8f,  -5.3f,  -5.9f,  -6.5f,  -7.1f,  -7.8f,  -8.5f,  -9.3f, -10.1f, -11.0f,
    -11.8f, -12.8f, -13.8f, -14.8f, -15.9f, -17.0f, -18.2f, -19.3f, -20.5f, -21.5f, -22.4f, -23.1f, -23.4f,
};
constexpr float kRolloff160[kPoints] = {
      0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,
      0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,
      0.0f,   0.0f,   0.0f,  -0.3f,  -0.6f,  -0.8f,  -1.0f,  -1.2f,  -1.5f,  -1.7f,  -2.1f,  -2.4f,  -2.8f,
     -3.1f,  -3.6f,  -4.1f,  -4.6f,  -5.1f,  -5.7f,  -6.3f,  -6.9f,  -7.6f,  -8.4f,  -9.1f,  -9.9f, -10.8f,
    -11.7f, -12.7f, -13.7f, -14.8f, -15.9f, -17.1f, -18.4f, -19.7f, -21.0f, -22.3f, -23.4f, -24.2f, -24.7f,
};
constexpr float kRolloff320[kPoints] = {
      0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,
      0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,
      0.0f,   0.0f,   0.0f,  -0.1f,  -0.2f,  -0.3f,  -0.5f,  -0.6f,  -0.8f,  -1.1f,  -1.4f,  -1.7f,  -2.0f,
     -2.4f,  -2.8f,  -3.2f,  -3.6f,  -4.1f,  -4.6f,  -5.1f,  -5.7f,  -6.3f,  -6.9f,  -7.6f,  -8.4f,  -9.2f,
    -10.1f, -11.0f, -12.0f, -13.0f, -14.0f, -15.2f, -16.4f, -17.8f, -19.1f, -20.5f, -21.7f, -22.6f, -23.1f,
};

constexpr unsigned kDesignPoints = 8192;  // frequency-sampling grid of the FIR design
constexpr double kKaiserBeta = 4;         // window of the 129 taps (<= 0.15 dB error to 0.95 x Nyquist)

std::mutex planner;  // FFTW's planner is not thread-safe

double interpolate(const float *table, double x)
{
    x = std::min(std::max(x, 0.0), 1.0) * (kPoints - 1);
    const unsigned k = std::min(unsigned(x), kPoints - 2);
    return table[k] + (x - k) * (table[k + 1] - table[k]);
}

double bessel_i0(double x)
{
    double sum = 1, term = 1;
    for (int k = 1; k < 50; ++k) {
        term *= (x / (2 * k)) * (x / (2 * k));
        sum += term;
    }
    return sum;
}

}  // namespace

const Flatten *Flatten::for_rate(double sample_rate)
{
    static std::mutex m;
    static std::map<long long, std::unique_ptr<Flatten>> made;
    const float *table = nullptr;
    const long long rate = std::llround(sample_rate);
    if (rate == 40000000) table = kRolloff40;
    else if (rate == 80000000) table = kRolloff80;
    else if (rate == 160000000) table = kRolloff160;
    else if (rate == 320000000) table = kRolloff320;
    if (!table) return nullptr;
    std::lock_guard<std::mutex> lock(m);
    auto &f = made[rate];
    if (!f) f.reset(new Flatten(double(rate), table));
    return f.get();
}

// Frequency sampling: the inverse of the roll-off (capped) on a fine grid, back to the time
// domain, the kTaps around zero kept under a Kaiser window. The gain is real and even, so the taps
// are real and symmetric: linear phase, delay kDelay.
Flatten::Flatten(double rate, const float *rolloff) : rate_(rate), rolloff_(rolloff), taps_(kTaps)
{
    std::vector<double> gain(kDesignPoints), impulse(kDesignPoints, 0.0);
    const double cap = std::pow(10.0, kMaxGainDb / 20);
    for (unsigned k = 0; k < kDesignPoints; ++k) {
        const double f = k < kDesignPoints / 2 ? double(k) / kDesignPoints : double(k) / kDesignPoints - 1;
        gain[k] = std::min(std::pow(10.0, -interpolate(rolloff, std::fabs(f) / 0.5) / 20), cap);
    }
    // The inverse DFT of a real even sequence: h[n] = (1/K) sum_k G[k] cos(2 pi k n / K).
    for (unsigned n = 0; n <= kDelay; ++n) {
        double sum = 0;
        for (unsigned k = 0; k < kDesignPoints; ++k) sum += gain[k] * std::cos(2 * M_PI * double(k) * n / kDesignPoints);
        impulse[n] = sum / kDesignPoints;
    }
    const double i0 = bessel_i0(kKaiserBeta);
    for (unsigned t = 0; t < kTaps; ++t) {
        const int n = int(t) - int(kDelay);
        const double r = double(n) / kDelay;
        taps_[t] = impulse[unsigned(std::abs(n))] * bessel_i0(kKaiserBeta * std::sqrt(std::max(0.0, 1 - r * r))) / i0;
    }
    // The transfer of the causal taps h[0..kTaps) for overlap-save, scaled by 1/kFft.
    transfer_.reset(fftwf_alloc_complex(kFft));
    fftwf_complex *in = fftwf_alloc_complex(kFft);
    for (unsigned k = 0; k < kFft; ++k) {
        in[k][0] = k < kTaps ? float(taps_[k] / kFft) : 0.0f;
        in[k][1] = 0;
    }
    {
        std::lock_guard<std::mutex> lock(planner);
        fftwf_plan p = fftwf_plan_dft_1d(int(kFft), in, transfer_.get(), FFTW_FORWARD, FFTW_ESTIMATE);
        fftwf_execute(p);
        fftwf_destroy_plan(p);
    }
    fftwf_free(in);
}

double Flatten::rolloff_db(double x) const { return interpolate(rolloff_, x); }

double Flatten::gain_db(double x) const
{
    const double w = M_PI * std::min(std::max(x, 0.0), 1.0);  // radians/sample
    double re = 0;
    for (unsigned t = 0; t < kTaps; ++t) re += taps_[t] * std::cos(w * (double(t) - kDelay));
    return 20 * std::log10(std::fabs(re));
}

FlattenRun::FlattenRun()
{
    buffer_ = fftwf_alloc_complex(Flatten::kFft);
    std::lock_guard<std::mutex> lock(planner);
    forward_ = fftwf_plan_dft_1d(int(Flatten::kFft), buffer_, buffer_, FFTW_FORWARD, FFTW_MEASURE);
    inverse_ = fftwf_plan_dft_1d(int(Flatten::kFft), buffer_, buffer_, FFTW_BACKWARD, FFTW_MEASURE);
    if (!forward_ || !inverse_) throw std::runtime_error("FFTW could not plan the correction's transforms");
}

FlattenRun::~FlattenRun()
{
    std::lock_guard<std::mutex> lock(planner);
    fftwf_destroy_plan(forward_);
    fftwf_destroy_plan(inverse_);
    fftwf_free(buffer_);
}

// Overlap-save: transform kFft inputs, multiply by the taps' transfer, transform back; the last
// kBlock outputs are free of wrap-around. Output t (0..n) is the full convolution at t + kTaps - 1,
// i.e. centred on input t + kDelay: zero-phase against the caller's indexing.
void FlattenRun::apply(const Flatten &f, const float *in, size_t n, float *out)
{
    const fftwf_complex *h = f.transfer();
    const size_t total = n + Flatten::kTaps - 1;
    for (size_t j = 0; j < n; j += Flatten::kBlock) {
        for (unsigned k = 0; k < Flatten::kFft; ++k) {
            const size_t i = j + k;
            buffer_[k][0] = i < total ? in[2 * i] : 0.0f;
            buffer_[k][1] = i < total ? in[2 * i + 1] : 0.0f;
        }
        fftwf_execute(forward_);
        for (unsigned k = 0; k < Flatten::kFft; ++k) {
            const float re = buffer_[k][0] * h[k][0] - buffer_[k][1] * h[k][1];
            const float im = buffer_[k][0] * h[k][1] + buffer_[k][1] * h[k][0];
            buffer_[k][0] = re;
            buffer_[k][1] = im;
        }
        fftwf_execute(inverse_);
        const size_t m = std::min<size_t>(Flatten::kBlock, n - j);
        for (size_t k = 0; k < m; ++k) {
            out[2 * (j + k)] = buffer_[Flatten::kTaps - 1 + k][0];
            out[2 * (j + k) + 1] = buffer_[Flatten::kTaps - 1 + k][1];
        }
    }
}

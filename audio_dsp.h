#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace audio {
constexpr int input_rate = 48000;
constexpr int input_frames = 3072;
constexpr int packet_bytes = 512;

// scipy.signal.resample_poly(x, 1, 6) 相当。
// 121タップ Kaiser FIR、beta=5。
// 元コード同様、3072サンプルごとに独立してゼロ埋め処理する。
class Converter {
    std::array<double, 121> taps_{};

    static double i0(double x) {
        double sum = 1.0;
        double term = 1.0;
        for (int k = 1; k < 100; ++k) {
            term *= (x * x / 4.0) / (k * k);
            sum += term;
            if (term < sum * 1e-16) {
                break;
            }
        }
        return sum;
    }

public:
    Converter() {
        constexpr double pi = 3.14159265358979323846;
        double sum = 0;

        for (int k = 0; k <= 120; ++k) {
            const double x = (k - 60) / 6.0;
            const double sinc = (k == 60 ? 1.0 : std::sin(pi * x) / (pi * x));
            const double r = (k - 60) / 60.0;

            taps_[k] =
                sinc / 6.0 * i0(5.0 * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0(5.0);

            sum += taps_[k];
        }

        for (auto& tap : taps_) tap /= sum;
    }

    std::array<std::uint8_t, packet_bytes>
    convert(const float* mono) const {
        std::array<std::uint8_t, packet_bytes> pcm{};

        for (int n = 0; n < packet_bytes; ++n) {
            float sample = 0;

            for (int k = 120; k >= 0; --k) {
                const int j = n * 6 + 60 - k;

                if (j >= 0 && j < input_frames) {
                    sample += static_cast<float>(taps_[k]) * mono[j];
                }
            }

            sample = (std::max)(-1.0f, (std::min)(sample, 1.0f));
            const float scaled = (sample + 1.0f) * 127.5f;

            // 偶数丸め。
            pcm[n] = static_cast<std::uint8_t>(std::nearbyint(scaled));
        }

        return pcm;
    }
};
}

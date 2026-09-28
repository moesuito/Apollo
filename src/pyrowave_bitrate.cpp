/**
 * @file src/pyrowave_bitrate.cpp
 * @brief Per-frame bitstream budget for the PyroWave encoder.
 */

// standard includes
#include <algorithm>
#include <cmath>

// third party includes
// Header-only regression from the codec author. Exposed by the
// pyrowave-regression-results CMake target and free of any Granite dependency,
// which is why it is used instead of the power law in bitrate-evaluation.md (the
// author marks that one obsolete).
#include <pyrowave_regression_results.h>

// local includes
#include "logging.h"
#include "pyrowave_bitrate.h"

namespace pyrowave {

  namespace {

    // The regression is only tabulated inside this range (see the header).
    constexpr int regression_min_psnr = PYROWAVE_REGRESSION_MIN_PSNR_HVS_M_H;
    constexpr int regression_max_psnr = PYROWAVE_REGRESSION_MAX_PSNR_HVS_M_H;
    constexpr int regression_min_pixels = PYROWAVE_REGRESSION_MIN_PIXELS;
    constexpr int regression_max_pixels = PYROWAVE_REGRESSION_MAX_PIXELS;

    // A frame is never allowed below one packet, otherwise packetize() has
    // nowhere to put the 8 byte frame header.
    constexpr std::size_t min_frame_bytes = 2048;

    // Sanity ceiling. At 3440x1440 the frame itself can reach several hundred KB,
    // but anything past this is a bug, not a legitimate budget.
    constexpr std::size_t max_frame_bytes = 8u * 1024u * 1024u;

    /**
     * @brief Estimate Mbit/s for a frame from the codec author's regression.
     */
    double estimate_mbits(double psnr, int width, int height, int fps) {
      auto pixels = static_cast<long long>(width) * height;

      // The regression asserts on out-of-range inputs, so clamp rather than
      // assert. Two cases matter here:
      //
      //  - Our target is 3440x1440 = 4.95 MP, which is inside the tabulated
      //    range (max 8.29 MP) but its 21:9 aspect ratio is *outside* what the
      //    author validated, so the number is an extrapolation (plan 4.5).
      //  - Low resolutions such as 1280x720 sit exactly on the lower bound.
      int clamped_pixels = static_cast<int>(std::clamp<long long>(pixels, regression_min_pixels, regression_max_pixels));
      int clamped_psnr = static_cast<int>(std::lround(std::clamp(psnr, static_cast<double>(regression_min_psnr), static_cast<double>(regression_max_psnr))));

      if (pixels != clamped_pixels) {
        BOOST_LOG(warning) << "PyroWave: " << pixels << " pixels is outside the regression range ["
                        << regression_min_pixels << ", " << regression_max_pixels << "], clamping to "
                        << clamped_pixels;
      }

      // height_factor 1.00 means the estimate already accounts for the actual
      // height, so no extra scaling is needed. Non-square pixels and anamorphic
      // sources are not a game streaming case.
      return pyrowave_psnr_hvs_m_h_estimate_mbits(
        clamped_psnr, clamped_pixels, height,
        PYROWAVE_HEIGHT_FACTOR_1_00, /*chroma444=*/ 0, static_cast<double>(fps));
    }

  }  // namespace

  std::size_t frame_bitstream_size(const bitrate_request_t &request, bool automatic, double psnr) {
    const int fps = std::max(1, request.fps);

    // Per-frame budget in bytes, before overhead.
    double bytes_per_frame = 0.0;

    if (automatic) {
      // The regression returns Mbit/s for the whole stream.
      auto mbits = estimate_mbits(psnr, request.width, request.height, fps);
      bytes_per_frame = (mbits * 1.0e6) / (8.0 * static_cast<double>(fps));
    }
    else {
      const double kbps = static_cast<double>(std::max(1, request.bitrate_kbps));
      bytes_per_frame = (kbps * 1000.0) / (8.0 * static_cast<double>(fps));
    }

    // The quality modifier is the Steam reference's continuous quality slider
    // (default 1.1). It scales the budget, so higher means more bytes and better
    // quality. Applied in both modes so the slider always has an effect.
    bytes_per_frame *= std::max(0.1, request.quality_modifier);

    // Account for the host's FEC overhead, if any, so the payload budget leaves
    // room for the parity data without pushing the total over the link budget.
    if (request.fec_percentage > 0) {
      bytes_per_frame *= (100.0 - std::min(99, request.fec_percentage)) / 100.0;
    }

    // Reserve headroom for the per-packet protocol headers. Moonlight spends 56
    // bytes per packet (IV 12 + GCM tag 16 + RTP 12 + NV_VIDEO_PACKET 16), and
    // the packet count follows from the payload size, so solving for the payload
    // that lands on the target total is a small fixed-point iteration.
    auto budget = static_cast<std::size_t>(bytes_per_frame);
    budget = std::clamp(budget, min_frame_bytes, max_frame_bytes);

    for (int i = 0; i < 8; ++i) {
      const std::size_t packets = (budget + request.packet_size - 1) / request.packet_size;
      if (packets == 0) {
        break;
      }
      const std::size_t with_headers = budget + packets * 56;
      const std::size_t adjusted = budget + (with_headers - budget);
      if (adjusted == budget) {
        break;
      }
      budget = std::clamp(adjusted, min_frame_bytes, max_frame_bytes);
    }

    BOOST_LOG(info) << "PyroWave: " << request.width << 'x' << request.height << '@' << fps
                    << " Hz budget " << budget << " B/frame ("
                    << (automatic ? "automatic" : "manual") << ", q=" << request.quality_modifier
                    << (automatic ? ", psnr=" + std::to_string(psnr) : "") << ')';

    return budget;
  }

}  // namespace pyrowave

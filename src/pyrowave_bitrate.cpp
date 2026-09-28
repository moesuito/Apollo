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

    // Sanity ceiling. Measured against the codec author's own regression
    // (regression_probe, see AGENTS.md 4.6), the largest legitimate budget is
    // 3840x2160 at 50 dB / 60 fps, which is 4185243 bytes per frame. This ceiling
    // sits above that, so it never binds on a real configuration - it exists only to
    // catch a nonsense config rather than to shape normal output.
    constexpr std::size_t max_frame_bytes = 6u * 1024u * 1024u;

    /**
     * @brief Estimate Mbit/s for a frame from the codec author's regression.
     */
    double estimate_mbits(double psnr, int width, int height, int fps) {
      // The regression takes a *resolution* and computes num_pixels = width * height
      // internally, so clamping the pixel count and passing it as the width would
      // multiply the area again. It asserts on out-of-range input, so the resolution
      // itself is scaled to fit while keeping the aspect ratio.
      //
      // Both ends of the range are reachable in practice: 1280x720 is exactly the
      // documented lower bound, and 3440x1440 (4.95 MP) is inside the range although
      // its 21:9 aspect is outside what the author validated, so that number is an
      // extrapolation (plan 4.5).
      auto pixels = static_cast<long long>(width) * height;
      int fit_width = width;
      int fit_height = height;

      if (pixels > regression_max_pixels) {
        fit_height = std::max(1, static_cast<int>(regression_max_pixels / width));
      }
      else if (pixels < regression_min_pixels) {
        fit_width = std::max(1, static_cast<int>(regression_min_pixels / height));
      }
      else {
        fit_width = width;
        fit_height = height;
      }

      if (fit_width != width || fit_height != height) {
        BOOST_LOG(warning) << "PyroWave: " << width << 'x' << height << " is outside the regression range ["
                        << regression_min_pixels << ", " << regression_max_pixels << "] pixels, "
                        << "estimating at " << fit_width << 'x' << fit_height << " instead";
      }

      const int clamped_psnr = static_cast<int>(std::lround(std::clamp(psnr, static_cast<double>(regression_min_psnr), static_cast<double>(regression_max_psnr))));

      if (psnr != clamped_psnr) {
        BOOST_LOG(warning) << "PyroWave: PSNR target " << psnr << " dB is outside the tabulated range ["
                        << regression_min_psnr << ", " << regression_max_psnr << "], using "
                        << clamped_psnr << " dB";
      }

      // height_factor 1.00 means the estimate already accounts for the actual height,
      // so no extra scaling is needed. Non-square pixels and anamorphic sources are
      // not a game streaming case.
      return pyrowave_psnr_hvs_m_h_estimate_mbits(
        clamped_psnr, fit_width, fit_height,
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

    auto budget = static_cast<std::size_t>(bytes_per_frame);
    budget = std::clamp(budget, min_frame_bytes, max_frame_bytes);

    // Moonlight spends 56 bytes per packet of protocol overhead (IV 12 + GCM tag 16 +
    // RTP 12 + NV_VIDEO_PACKET 16), and the packet count follows from the payload
    // size. Since the overhead is ~4% of a 1392-byte packet, folding it in as a
    // division rather than an iteration is accurate to well under a percent and
    // cannot diverge: dividing by (1 - 56/packet_size) always moves the budget down,
    // so there is no fixed point to search for.
    if (request.packet_size > 56) {
      const double overhead_factor = static_cast<double>(request.packet_size) /
                                     static_cast<double>(request.packet_size - 56);
      budget = static_cast<std::size_t>(static_cast<double>(budget) / overhead_factor);
      budget = std::clamp(budget, min_frame_bytes, max_frame_bytes);
    }

    BOOST_LOG(info) << "PyroWave: " << request.width << 'x' << request.height << '@' << fps
                    << " Hz budget " << budget << " B/frame ("
                    << (automatic ? "automatic" : "manual") << ", q=" << request.quality_modifier
                    << (automatic ? ", psnr=" + std::to_string(psnr) : "") << ')';

    return budget;
  }

}  // namespace pyrowave

/**
 * @file src/pyrowave_bitrate.h
 * @brief Per-frame bitstream budget for the PyroWave encoder.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace pyrowave {

  /**
   * @brief Inputs needed to turn a stream configuration into a byte budget.
   */
  struct bitrate_request_t {
    int width = 0;
    int height = 0;
    int fps = 0;
    int bitrate_kbps = 0;  ///< Total video bitrate in kilobits per second.
    double quality_modifier = 1.1;  ///< Steam reference default.
    std::size_t packet_size = 1392;  ///< Moonlight's default packetSize.
    int fec_percentage = 0;  ///< Host's FEC overhead, if any.
  };

  /**
   * @brief Bytes the encoder may use for one frame.
   *
   * PyroWave exposes exactly one rate control knob: a hard per-frame byte ceiling
   * (pyrowave_rate_control::maximum_bitstream_size). There is no bitrate, no QP,
   * no CRF, no VBR/CBR, and nothing adaptive - see plan 4.1. All of the
   * intelligence has to live here.
   *
   * "Automatic" mode derives the budget from the codec author's own regression
   * (pyrowave_psnr_hvs_m_h_estimate_mbits, header only, exposed by the
   * pyrowave-regression-results target) rather than from the obsolete power law in
   * bitrate-evaluation.md, which the author marks as superseded.
   *
   * Manual mode scales the requested bitrate by the quality modifier.
   *
   * @param request The stream configuration.
   * @param automatic Whether to use the regression instead of the raw bitrate.
   * @param psnr Target PSNR in dB, only used when automatic is true.
   * @return A per-frame byte budget. Always at least one packet, so a frame is
   *         never smaller than the MTU.
   */
  std::size_t frame_bitstream_size(const bitrate_request_t &request, bool automatic, double psnr);

}  // namespace pyrowave

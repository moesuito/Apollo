/**
 * @file src/pyrowave_encode.h
 * @brief PyroWave encode session for the Apollo host.
 *
 * PyroWave is not an FFmpeg codec, so it cannot ride in
 * avcodec_encode_session_t. It gets its own path in the same style as the
 * standalone NVENC session, which is also a dynamic_cast sibling rather than an
 * avcodec one. See video.cpp make_encode_session() and encode().
 */
#pragma once

// local includes
#include "pyrowave_codec.h"
#include "video.h"

// standard includes
#include <cstdint>
#include <memory>
#include <vector>

namespace video {

  class pyrowave_encode_session_t: public encode_session_t {
  public:
    pyrowave_encode_session_t(
      pyrowave::device_t device,
      pyrowave::sync_object_t sync,
      int width,
      int height,
      const config_t &config,
      std::size_t packet_size,
      int fec_percentage,
      bool automatic_bitrate,
      double quality_modifier,
      double target_psnr);

    ~pyrowave_encode_session_t() override = default;

    /**
     * @brief Set the source image for the next encode.
     *
     * PyroWave does the RGB to YCbCr conversion and scaling on the GPU itself (via
     * pyrowave_scaled_encode_info), so this only has to import the D3D11 texture
     * and hand it over. That is why there is no conversion shader here, unlike
     * every other encoder in this file.
     *
     * @param img The captured image.
     * @return 0 on success.
     */
    int convert(platf::img_t &img) override;

    /**
     * @brief Encode and packetize, then push the frame to the packet queue.
     *
     * @param frame_nr Monotonic frame counter.
     * @param packets Queue the encoded packet is raised onto.
     * @param channel_data Opaque value passed back to the packet consumer.
     * @param frame_timestamp Capture timestamp, for latency accounting.
     * @return 0 on success.
     */
    int encode_frame(
      int64_t frame_nr,
      safe::mail_raw_t::queue_t<packet_t> &packets,
      void *channel_data,
      std::optional<std::chrono::steady_clock::time_point> frame_timestamp);

    /**
     * @brief No-op: every PyroWave frame is an IDR.
     *
     * The codec is intra-only with no GOP, no keyframe request, and no lookahead.
     * Reporting otherwise would make the host wait forever for an IDR that the
     * encoder has no concept of.
     */
    void request_idr_frame() override {
    }

    /**
     * @brief No-op, for the same reason as request_idr_frame().
     */
    void request_normal_frame() override {
    }

    /**
     * @brief No-op: there are no reference frames to invalidate.
     *
     * Reference frame invalidation exists to avoid re-sending a large intra frame
     * after damage. With intra-only encoding every frame is already intra, so the
     * concept does not apply.
     */
    void invalidate_ref_frames(int64_t first_frame, int64_t last_frame) override {
    }

    /**
     * @brief Handle a resolution change by recreating the encoder.
     *
     * The encoder is bound to a fixed frame size at creation time, and the
     * client's resolution can change mid-stream.
     */
    void set_resolution(int width, int height);

  private:
    void refresh_rate_control();

    pyrowave::device_t m_device;
    pyrowave::sync_object_t m_sync;
    pyrowave::image_t m_image;

    int m_width;
    int m_height;
    std::size_t m_packet_size;
    int m_fec_percentage;

    config_t m_config;

    // Declared before m_encoder so the member initializer order matches the
    // declaration order; the encoder is created in the constructor body because it
    // depends on the device.
    std::uint64_t m_timeline = 0;
    bool m_automatic_bitrate = true;
    double m_target_psnr = 40.0;
    double m_quality_modifier = 1.1;

    pyrowave::encoder_t m_encoder;
    pyrowave::bitstream_t m_bitstream;
    std::vector<pyrowave::packet_t> m_packets;
  };

}  // namespace video

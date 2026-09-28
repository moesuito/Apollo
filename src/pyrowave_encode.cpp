/**
 * @file src/pyrowave_encode.cpp
 * @brief PyroWave encode session for the Apollo host.
 */

// standard includes
#include <algorithm>
#include <utility>

// local includes
#include "src/logging.h"
#include "src/pyrowave_bitrate.h"
#include "src/pyrowave_encode.h"

#ifdef _WIN32
  // standard includes
  #include <d3d11_4.h>

  // local includes
  // Declares capture_texture_of(), whose definition sits next to the private
  // img_d3d_t in display_vram.cpp.
  #include "src/platform/windows/pyrowave_device.h"
  #include "src/pyrowave_protocol.h"

  // third party includes
  // ComPtr from the PyroWave repo, used here only for the short-lived
  // CreateSharedHandle interop below. Apache/MIT compatible.
  #include <com_ptr.hpp>
#endif

namespace video {

#ifdef _WIN32

  pyrowave_encode_session_t::pyrowave_encode_session_t(
    pyrowave::device_t device,
    pyrowave::sync_object_t sync,
    int width,
    int height,
    const config_t &config,
    std::size_t packet_size,
    int fec_percentage,
    bool automatic_bitrate,
    double quality_modifier,
    double target_psnr):
    m_device {std::move(device)},
    m_sync {std::move(sync)},
    m_width {width},
    m_height {height},
    m_packet_size {packet_size},
    m_fec_percentage {fec_percentage},
    m_config {config},
    m_automatic_bitrate {automatic_bitrate},
    m_target_psnr {target_psnr},
    m_quality_modifier {quality_modifier} {
    m_encoder = pyrowave::encoder_t::create(m_device, width, height, pyrowave::chroma_e::yuv420);
    if (!m_encoder) {
      BOOST_LOG(error) << "PyroWave: failed to create encoder"sv;
      return;
    }

    m_encoder.set_sync_object(m_sync);

    // Start with a conservative budget; it is recomputed on every frame by
    // refresh_rate_control() so config changes and the framerate take effect
    // without recreating the encoder.
    refresh_rate_control();
  }

  void pyrowave_encode_session_t::refresh_rate_control() {
    pyrowave::bitrate_request_t request;
    request.width = m_width;
    request.height = m_height;
    request.fps = m_config.framerate > 0 ? m_config.framerate : 60;
    request.bitrate_kbps = m_config.bitrate;
    request.quality_modifier = m_quality_modifier;
    request.packet_size = m_packet_size;
    request.fec_percentage = m_fec_percentage;

    const auto budget = pyrowave::frame_bitstream_size(request, m_automatic_bitrate, m_target_psnr);
    m_encoder.set_rate_control_size(budget);
  }

  int pyrowave_encode_session_t::convert(platf::img_t &img) {
    bool bgra = true;
    bool valid = false;
    auto *texture = capture_texture_of(img, bgra, valid);

    if (!valid || !texture) {
      return -1;
    }

    // The codec needs a shared NT handle. pyrowave.h explicitly recommends
    // DuplicateHandle() here to work around driver quirks, and we must duplicate
    // regardless because the codec takes ownership and closes the handle while
    // the capture texture outlives this call.
    ComPtr<IDXGIResource1> resource;
    if (FAILED(texture->QueryInterface(IID_IDXGIResource1, resource.ppv()))) {
      BOOST_LOG(error) << "PyroWave: failed to get IDXGIResource1 from capture texture"sv;
      return -1;
    }

    HANDLE shared_handle = nullptr;
    if (FAILED(resource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &shared_handle))) {
      BOOST_LOG(error) << "PyroWave: CreateSharedHandle failed"sv;
      return -1;
    }

    // The device takes ownership of shared_handle on success, so this guard only
    // covers the failure path below.
    struct handle_guard_t {
      HANDLE handle;
      ~handle_guard_t() {
        if (handle) {
          CloseHandle(handle);
        }
      }
    } guard {shared_handle};

    if (pyrowave::image_from_d3d11_texture(m_device, shared_handle, bgra, m_image) != pyrowave::result_e::ok) {
      return -1;
    }

    guard.handle = nullptr;
    return 0;
  }

  int pyrowave_encode_session_t::encode_frame(
    int64_t frame_nr,
    safe::mail_raw_t::queue_t<packet_t> &packets,
    void *channel_data,
    std::optional<std::chrono::steady_clock::time_point> frame_timestamp) {

    if (!m_encoder || !m_image) {
      BOOST_LOG(error) << "PyroWave: encode called without a valid encoder or image"sv;
      return -1;
    }

    refresh_rate_control();

    // Two timeline values per frame: the encoder waits for `acquire` before
    // reading the capture texture, and signals `release` when it is done. The
    // capture thread signals `acquire` on the shared fence. The +1 is because a
    // timeline semaphore's value must be strictly greater than the one it last
    // signalled.
    const std::uint64_t acquire = ++m_timeline;
    const std::uint64_t release = ++m_timeline;

    if (m_encoder.encode_from_image(m_image, acquire, release) != pyrowave::result_e::ok) {
      return -1;
    }

    std::size_t num_packets = 0;
    if (m_encoder.compute_num_packets(m_packet_size, num_packets) != pyrowave::result_e::ok) {
      return -1;
    }

    // packetize() needs a buffer at least as large as the encoded frame plus
    // slack, because packet_boundary is a soft target and one packet can exceed
    // it.
    m_bitstream.grow_to(m_encoder.encoded_size() + m_packet_size);

    if (m_encoder.packetize(m_packet_size, m_bitstream, m_packets) != pyrowave::result_e::ok) {
      return -1;
    }

    if (m_packets.empty()) {
      BOOST_LOG(error) << "PyroWave: packetize produced no packets"sv;
      return -1;
    }

    // The Moonlight packetizer consumes one contiguous blob and slices it itself,
    // the same way the AV1 path works. Concatenating the packets is therefore
    // correct, and is what packet_raw_generic was built for.
    std::vector<std::uint8_t> frame_data;
    frame_data.reserve(m_encoder.encoded_size());
    for (const auto &p : m_packets) {
      const auto *begin = m_bitstream.data() + p.offset;
      frame_data.insert(frame_data.end(), begin, begin + p.size);
    }

    if (frame_data.empty()) {
      BOOST_LOG(error) << "PyroWave: empty frame after packet assembly"sv;
      return -1;
    }

    // Every PyroWave frame is an IDR. is_idr() must say so unconditionally, or
    // the host's IDR bookkeeping will misbehave.
    auto packet = std::make_unique<packet_raw_generic>(std::move(frame_data), frame_nr, true);
    packet->channel_data = channel_data;
    packet->frame_timestamp = frame_timestamp;
    // No replacements: unlike H.264/HEVC there is no SPS/PPS to inject, so this
    // stays null exactly as it does for AV1.
    packet->replacements = nullptr;
    packets->raise(std::move(packet));

    m_image = {};
    return 0;
  }

  void pyrowave_encode_session_t::set_resolution(int width, int height) {
    if (width == m_width && height == m_height) {
      return;
    }

    BOOST_LOG(info) << "PyroWave: resolution change " << m_width << 'x' << m_height
                    << " -> " << width << 'x' << height;

    m_width = width;
    m_height = height;
    m_encoder = pyrowave::encoder_t::create(m_device, width, height, pyrowave::chroma_e::yuv420);
    if (m_encoder) {
      m_encoder.set_sync_object(m_sync);
      refresh_rate_control();
    }
  }

#endif  // _WIN32

}  // namespace video

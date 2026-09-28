/**
 * @file src/pyrowave_codec.cpp
 * @brief Implementation of the PyroWave C API wrapper.
 *
 * This is the ONLY translation unit in Apollo that includes <pyrowave.h>. If a
 * breaking upstream change lands, this file absorbs it (AGENTS.md rule 6,
 * plan risk R6).
 */

// standard includes
#include <cstring>

// third party includes
// pyrowave.h hard-errors unless the Vulkan core header came first (its own
// #error at the top of the file), so the order here is a requirement, not a
// preference.
#include <vulkan/vulkan.h>

extern "C" {
#include <pyrowave.h>
}

// local includes
#include "logging.h"
#include "pyrowave_codec.h"

namespace pyrowave {

  namespace {

    /**
     * @brief Log a failed codec call and map it to result_e::error.
     */
    result_e check(::pyrowave_result result, const char *what) {
      if (result != PYROWAVE_SUCCESS) {
        BOOST_LOG(error) << "PyroWave: " << what << " failed, result " << static_cast<int>(result);
        return result_e::error;
      }
      return result_e::ok;
    }

  }  // namespace

  // The casts are safe because each handle can only have come out of the
  // matching create function; the void* signature is what keeps <vulkan.h> out of
  // the public header.

  void device_ops::destroy(void *handle) {
    ::pyrowave_device_destroy(reinterpret_cast<::pyrowave_device>(handle));
  }

  void image_ops::destroy(void *handle) {
    ::pyrowave_image_destroy(reinterpret_cast<::pyrowave_image>(handle));
  }

  void sync_object_ops::destroy(void *handle) {
    ::pyrowave_sync_object_destroy(reinterpret_cast<::pyrowave_sync_object>(handle));
  }

  void encoder_ops::destroy(void *handle) {
    ::pyrowave_encoder_destroy(reinterpret_cast<::pyrowave_encoder>(handle));
  }

  void bitstream_t::grow_to(std::size_t capacity) {
    if (capacity > m_data.size()) {
      m_data.resize(capacity);
    }
  }

  device_t device_t::create_for_adapter(const std::uint8_t *luid) {
    ::pyrowave_device device = nullptr;

    // API 0.6.0 spelling: (vid, pid, device_id, device_node, luid, out_device).
    // Zero vendor/device ids mean "no preference", letting the LUID select. A
    // null LUID lets the codec pick the first suitable device, which is wrong on
    // a hybrid-graphics laptop.
    auto *luid_ptr = luid ? reinterpret_cast<const ::pyrowave_luid *>(luid) : nullptr;

    if (check(::pyrowave_create_device_by_compat(0, 0, 0, 0, luid_ptr, &device), "create_device_by_compat") != result_e::ok) {
      return {};
    }

    return device_t {reinterpret_cast<void *>(device)};
  }

  result_e image_from_d3d11_texture(const device_t &device, void *shared_handle, bool bgra, int width, int height, image_t &image) {
    auto *pyro_device = reinterpret_cast<::pyrowave_device>(device.get());

    // pyrowave_image_create validates this struct rather than deriving it from the
    // handle: it rejects a null pointer outright, and separately checks tiling,
    // imageType and sharingMode. The values below mirror the working reference in
    // pyrowave/encode_desktop.cpp, which imports a DXGI-captured desktop texture -
    // the same kind of resource the host passes in.
    VkImageCreateInfo create_info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    create_info.imageType = VK_IMAGE_TYPE_2D;
    create_info.format = bgra ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_B8G8R8A8_UNORM;
    create_info.extent = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 1};
    create_info.mipLevels = 1;
    create_info.arrayLayers = 1;
    create_info.samples = VK_SAMPLE_COUNT_1_BIT;
    create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    create_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    ::pyrowave_image_create_info info = {};
    info.device = pyro_device;
    info.external_handle = reinterpret_cast<::pyrowave_os_handle>(shared_handle);
    info.handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    info.image_create_info = &create_info;

    ::pyrowave_image pyro_image = nullptr;
    if (check(::pyrowave_image_create(&info, &pyro_image), "image_create") != result_e::ok) {
      return result_e::error;
    }

    image = image_t {reinterpret_cast<void *>(pyro_image)};
    return result_e::ok;
  }

  sync_object_t sync_object_t::create_own(const device_t &device) {
    auto *pyro_device = reinterpret_cast<::pyrowave_device>(device.get());

    ::pyrowave_sync_object_create_info info = {};
    info.device = pyro_device;
    // A zero external_handle asks the codec to create a fresh, exportable timeline
    // semaphore rather than import one. pyrowave_c.cpp rejects that combination
    // with PYROWAVE_ERROR_INVALID_ARGUMENT unless IMPORT_TEMPORARY is set, so the
    // flag is mandatory here rather than a choice.
    info.external_handle = 0;
    info.handle_type = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    info.semaphore_type = VK_SEMAPHORE_TYPE_TIMELINE;
    info.import_flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;

    ::pyrowave_sync_object pyro_sync = nullptr;
    if (check(::pyrowave_sync_object_create(&info, &pyro_sync), "sync_object_create (own)") != result_e::ok) {
      return {};
    }

    return sync_object_t {reinterpret_cast<void *>(pyro_sync)};
  }

  result_e sync_object_from_d3d11_fence(const device_t &device, void *fence_handle, sync_object_t &sync) {
    auto *pyro_device = reinterpret_cast<::pyrowave_device>(device.get());

    ::pyrowave_sync_object_create_info info = {};
    info.device = pyro_device;
    info.external_handle = reinterpret_cast<::pyrowave_os_handle>(fence_handle);
    info.handle_type = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT;
    info.semaphore_type = VK_SEMAPHORE_TYPE_TIMELINE;

    ::pyrowave_sync_object pyro_sync = nullptr;
    if (check(::pyrowave_sync_object_create(&info, &pyro_sync), "sync_object_create") != result_e::ok) {
      return result_e::error;
    }

    sync = sync_object_t {reinterpret_cast<void *>(pyro_sync)};
    return result_e::ok;
  }

  encoder_t encoder_t::create(const device_t &device, int width, int height, chroma_e chroma) {
    auto *pyro_device = reinterpret_cast<::pyrowave_device>(device.get());

    ::pyrowave_encoder_create_info info = {};
    info.device = pyro_device;
    info.width = width;
    info.height = height;
    info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
    static_cast<void>(chroma);

    ::pyrowave_encoder pyro_encoder = nullptr;
    if (check(::pyrowave_encoder_create(&info, &pyro_encoder), "encoder_create") != result_e::ok) {
      return {};
    }

    return encoder_t {reinterpret_cast<void *>(pyro_encoder)};
  }

  void encoder_t::set_sync_object(const sync_object_t &sync) {
    m_sync = sync.get();
  }

  void encoder_t::set_rate_control_size(std::size_t bytes) {
    m_rate_control_size = bytes;
  }

  result_e encoder_t::encode_from_image(const image_t &image, std::uint64_t acquire, std::uint64_t release) {
    auto *pyro_encoder = reinterpret_cast<::pyrowave_encoder>(get());
    auto *pyro_image = reinterpret_cast<::pyrowave_image>(image.get());

    ::pyrowave_gpu_external_reference ref = {};
    ref.image = pyro_image;
    ref.queue_family_index = VK_QUEUE_FAMILY_EXTERNAL;

    ::pyrowave_gpu_sync_operation acquire_op = {};
    acquire_op.images = &ref;
    acquire_op.num_images = 1;

    ::pyrowave_gpu_sync_operation release_op = {};

    // The scaled path is what keeps this integration small: it performs the
    // RGB -> YCbCr conversion and any rescaling on the GPU, so Apollo needs no
    // conversion shader of its own. This removes plan task 2.2 entirely.
    ::pyrowave_scaled_encode_info info = {};
    if (check(::pyrowave_image_get_image_view(pyro_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_USAGE_SAMPLED_BIT, &info.view), "image_get_image_view") != result_e::ok) {
      return result_e::error;
    }

    // SDR for now. HDR would need the client's dynamicRange to drive this plus a
    // 10-bit input image; neither is wired up, and the Steam reference defaults
    // to SDR.
    info.input_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    info.output_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    // R8_UNORM gets dithered by the encoder, which smooths out banding. R16 would
    // be needed for HDR10 but is not usable here since this path is SDR.
    info.intermediate_plane_format = VK_FORMAT_R8_UNORM;
    // Full-range YCbCr with center chroma siting; the codec's transform is fixed,
    // so this is informational rather than configurable.
    info.ycbcr_chroma_midpoint = 128.0f / 255.0f;

    ::pyrowave_rate_control rate_control = {};
    rate_control.maximum_bitstream_size = m_rate_control_size;

    if (m_sync) {
      auto *sync = reinterpret_cast<::pyrowave_sync_object>(m_sync);
      auto semaphore = ::pyrowave_sync_object_get_semaphore(sync);
      acquire_op.sync.semaphore = semaphore;
      acquire_op.sync.value = acquire;
      release_op.sync.semaphore = semaphore;
      release_op.sync.value = release;
    }

    if (check(::pyrowave_encoder_encode_gpu_scaled_synchronous(pyro_encoder, &acquire_op, &release_op, &info, &rate_control), "encode_gpu_scaled_synchronous") != result_e::ok) {
      return result_e::error;
    }

    m_has_encoded_frame = true;
    m_encoded_size = rate_control.maximum_bitstream_size;
    return result_e::ok;
  }

  result_e encoder_t::compute_num_packets(std::size_t packet_boundary, std::size_t &num_packets) {
    auto *pyro_encoder = reinterpret_cast<::pyrowave_encoder>(get());
    return check(::pyrowave_encoder_compute_num_packets(pyro_encoder, packet_boundary, &num_packets), "compute_num_packets");
  }

  result_e encoder_t::packetize(std::size_t packet_boundary, bitstream_t &bitstream, std::vector<packet_t> &packets) {
    if (!m_has_encoded_frame) {
      BOOST_LOG(error) << "PyroWave: packetize() called before a successful encode";
      return result_e::error;
    }

    auto *pyro_encoder = reinterpret_cast<::pyrowave_encoder>(get());

    // packetize writes into a caller-provided array of pyrowave_packet. Our
    // packet_t is layout-compatible (two size_t) but we do not rely on that:
    // build a vector of the real type, then copy out.
    std::vector<::pyrowave_packet> raw_packets;
    raw_packets.resize(64);

    // The worst case is one oversized packet (packet_boundary is a soft target),
    // so guarantee room for the whole frame plus one boundary's worth of slack.
    bitstream.grow_to(m_encoded_size + packet_boundary);

    std::size_t out_packets = 0;
    if (check(::pyrowave_encoder_packetize(pyro_encoder, raw_packets.data(), packet_boundary, &out_packets, bitstream.data(), bitstream.size()), "packetize") != result_e::ok) {
      return result_e::error;
    }

    raw_packets.resize(out_packets);
    packets.resize(out_packets);
    for (std::size_t i = 0; i < out_packets; ++i) {
      packets[i].offset = raw_packets[i].offset;
      packets[i].size = raw_packets[i].size;
    }

    // The frame is only truly encoded-size bytes long; the rest of the buffer is
    // slack and must not be reported to the packetizer as payload.
    bitstream.set_size(m_encoded_size);
    m_has_encoded_frame = false;
    return result_e::ok;
  }

}  // namespace pyrowave

/**
 * @file src/pyrowave_codec.h
 * @brief Thin C++ wrapper around the PyroWave C API.
 *
 * WHY THIS EXISTS (AGENTS.md rule 6 / plan risk R6): the PyroWave API is version
 * 0.6.0 and its author states plainly that "API and ABI is not stable until MAJOR
 * hits 1". Nothing outside this header/cpp may include <pyrowave.h> or call a
 * `pyrowave_*` function directly. A breaking upstream change then becomes a
 * localized edit here instead of a sweep through the Apollo encoder.
 *
 * This wrapper is deliberately thin. It owns lifetime, error reporting, and the
 * encode/packetize state machine, and adds no abstraction of its own.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace pyrowave {

  /**
   * @brief Result of a codec call. The codec has many failure modes, so this
   *        collapses them into ok / error while logging the detail.
   */
  enum class result_e {
    ok,
    error,
  };

  /**
   * @brief Chroma subsampling. Only 4:2:0 is implemented in this integration.
   *
   * PyroWave's yuv444p is not usable for YCbCr sampling on this hardware
   * (validated: AGENTS.md 4.1), and Moonlight-Qt's renderers expect a single
   * NV12-like texture rather than 3 separate planes. 4:2:0 8-bit is also the
   * Steam reference default, so it is the right starting point.
   */
  enum class chroma_e {
    yuv420,
  };

  // Typed destruction, kept behind a void* signature so <vulkan.h> never leaks
  // into this header. The .cpp casts back to the real opaque codec type, which is
  // what makes this safe: the cast is only ever applied to a handle that came out
  // of the matching create function.
  struct device_ops {
    static void destroy(void *handle);
  };
  struct image_ops {
    static void destroy(void *handle);
  };
  struct sync_object_ops {
    static void destroy(void *handle);
  };
  struct encoder_ops {
    static void destroy(void *handle);
  };

  /**
   * @brief Owning handle wrapper. Calls the matching destroy on destruction, so
   *        codec objects cannot leak on any error path.
   *
   * @c DestroyFn takes void* so this header needs no Vulkan types; the ops structs
   * above are where the real typed destroy call lives.
   */
  template<void (*DestroyFn)(void *)>
  class handle_t {
  public:
    handle_t() = default;
    explicit handle_t(void *handle): m_handle {handle} {}
    ~handle_t() {
      if (m_handle) {
        DestroyFn(m_handle);
      }
    }

    handle_t(const handle_t &) = delete;
    handle_t &operator=(const handle_t &) = delete;

    handle_t(handle_t &&other) noexcept: m_handle {other.m_handle} {
      other.m_handle = nullptr;
    }

    handle_t &operator=(handle_t &&other) noexcept {
      if (this != &other) {
        if (m_handle) {
          DestroyFn(m_handle);
        }
        m_handle = other.m_handle;
        other.m_handle = nullptr;
      }
      return *this;
    }

    [[nodiscard]] void *get() const {
      return m_handle;
    }

    [[nodiscard]] explicit operator bool() const {
      return m_handle != nullptr;
    }

    // So `if (!handle)` reads naturally; the explicit operator bool() above is
    // not usable in a unary !.
    [[nodiscard]] bool operator!() const {
      return m_handle == nullptr;
    }

    /**
     * @brief Relinquish ownership, returning the raw handle.
     *
     * Used where a handle has to be moved behind a void* boundary to keep
     * <vulkan.h> out of a platform header. The caller becomes responsible for
     * calling the matching destroy.
     */
    [[nodiscard]] void *release_handle() {
      auto handle = m_handle;
      m_handle = nullptr;
      return handle;
    }

  private:
    void *m_handle = nullptr;
  };

  class device_t;
  class image_t;
  class sync_object_t;
  class encoder_t;

  /**
   * @brief One packet of a packetized bitstream, as produced by packetize().
   */
  struct packet_t {
    std::size_t offset = 0;
    std::size_t size = 0;
  };

  /**
   * @brief A bitstream buffer sized to hold one worst-case frame.
   *
   * The codec has no flush() and no way to report the size it needs up front, so
   * the buffer starts at the rate control budget and grows if packetize() needs
   * more.
   */
  class bitstream_t {
  public:
    bitstream_t() = default;

    explicit bitstream_t(std::size_t capacity) {
      m_data.resize(capacity);
    }

    [[nodiscard]] std::uint8_t *data() {
      return m_data.data();
    }
    [[nodiscard]] std::size_t size() const {
      return m_size;
    }
    [[nodiscard]] std::size_t capacity() const {
      return m_data.size();
    }
    void set_size(std::size_t size) {
      m_size = size;
    }
    void grow_to(std::size_t capacity);

  private:
    std::vector<std::uint8_t> m_data;
    std::size_t m_size = 0;
  };

  /**
   * @brief A Vulkan device created through the PyroWave API.
   */
  class device_t final: public handle_t<device_ops::destroy> {
  public:
    device_t() = default;

    // Inherits handle_t's void* constructor, so a create_* factory can return the
    // handle directly without a second forwarding constructor.
    using handle_t::handle_t;

    /**
     * @brief Create a device bound to a DXGI adapter LUID.
     *
     * This is how the host binds the encoder to the same physical GPU the desktop
     * is captured from. Passing a null LUID lets the codec pick the first suitable
     * device, which is wrong on hybrid-graphics laptops (this machine has both an
     * RTX 3060 and Intel UHD).
     *
     * @param luid The 8-byte DXGI adapter LUID, or null to let the codec choose.
     */
    static device_t create_for_adapter(const std::uint8_t *luid);
  };

  /**
   * @brief A Vulkan image imported from an external handle.
   */
  class image_t final: public handle_t<image_ops::destroy> {
  public:
    image_t() = default;

    using handle_t::handle_t;
  };

  /**
   * @brief A timeline semaphore imported from an external handle.
   *
   * On Windows the host imports the ID3D11Fence it already uses to synchronize
   * capture, rather than allocating a second sync primitive. The working
   * reference is pyrowave/encode_desktop.cpp in the PyroWave repo.
   */
  class sync_object_t final: public handle_t<sync_object_ops::destroy> {
  public:
    sync_object_t() = default;

    using handle_t::handle_t;

    /**
     * @brief Create a timeline semaphore with no external producer.
     *
     * The encoder API requires a sync point to be supplied and signalled even when
     * nothing else is racing it. The capture textures carry their own keyed mutex,
     * so the host is the only party synchronizing and a host-owned semaphore is
     * enough.
     *
     * @note Prefer sync_object_from_d3d11_fence() when the caller already has a
     *       D3D11 fence to share, to avoid a second sync primitive.
     */
    static sync_object_t create_own(const device_t &device);
  };

  /**
   * @brief The PyroWave encoder.
   *
   * Not thread safe, per the codec's own documentation. The Apollo video encode
   * thread is single threaded, so no locking is needed here.
   */
  class encoder_t final: public handle_t<encoder_ops::destroy> {
  public:
    encoder_t() = default;

    using handle_t::handle_t;

    /**
     * @brief Create an encoder on a device.
     * @param width Encoded width. Must be even for 4:2:0.
     * @param height Encoded height. Must be even for 4:2:0.
     */
    static encoder_t create(const device_t &device, int width, int height, chroma_e chroma);

    /**
     * @brief Attach the sync object used for acquire/release handshakes.
     *
     * Optional: without one, encode() runs unsynchronized, which is only correct
     * when the caller can guarantee the image is not being written concurrently.
     */
    void set_sync_object(const sync_object_t &sync);

    /**
     * @brief Set the per-frame byte ceiling for the next encode.
     *
     * This is the codec's only rate control knob (see plan 4.1): a hard maximum
     * bitstream size in bytes, not a target bitrate. The caller computes it.
     */
    void set_rate_control_size(std::size_t bytes);

    /**
     * @brief Encode a single frame from an imported image.
     *
     * Synchronous by design: PyroWave is intra-only and fast enough that
     * overlapping frames would only add latency. There is no flush(), so one call
     * produces exactly one frame.
     *
     * @param image Source image, from image_from_d3d11_texture().
     * @param acquire Timeline value to wait for before reading the image.
     * @param release Timeline value to signal when the read is done.
     */
    result_e encode_from_image(const image_t &image, std::uint64_t acquire, std::uint64_t release);

    /**
     * @brief Number of packets the last encoded frame needs at a given MTU.
     */
    result_e compute_num_packets(std::size_t packet_boundary, std::size_t &num_packets);

    /**
     * @brief Split the last encoded frame into MTU-sized packets.
     *
     * @p packet_boundary is a soft target, not a hard ceiling. The PyroWave
     * bitstream is built from self-delimiting 32x32 blocks, so a block larger
     * than the boundary yields an oversized packet rather than an error. Callers
     * must tolerate that (Moonlight's max packet size is comfortably above the
     * ~1.4 KB boundary we use).
     */
    result_e packetize(std::size_t packet_boundary, bitstream_t &bitstream, std::vector<packet_t> &packets);

    /**
     * @brief Bytes the last encoded frame actually occupied.
     *
     * This is the host's lastPayloadLen. It is mandatory: the Moonlight
     * packetizer pads the final packet to the boundary, and zero padding would
     * corrupt the frame because PyroWave has no length field per packet.
     */
    [[nodiscard]] std::size_t encoded_size() const {
      return m_encoded_size;
    }

    /**
     * @brief True once a frame has been encoded and not yet packetized.
     */
    [[nodiscard]] bool has_encoded_frame() const {
      return m_has_encoded_frame;
    }

  private:
    std::size_t m_rate_control_size = 0;
    std::size_t m_encoded_size = 0;
    bool m_has_encoded_frame = false;
    void *m_sync = nullptr;
  };

  /**
   * @brief Import a D3D11 texture as a PyroWave image.
   *
   * The codec takes ownership of @p shared_handle on success and closes it, so
   * the caller must pass a handle it has duplicated and no longer needs. The
   * header's own comment in pyrowave.h suggests DuplicateHandle() for exactly
   * this reason.
   *
   * @param shared_handle A shared handle from IDXGIResource1::CreateSharedHandle.
   * @param bgra Whether the source is DXGI_FORMAT_B8G8R8A8_UNORM. Kept explicit
   *             because the capture path can also deliver R16G16B16A16_FLOAT.
   */
  result_e image_from_d3d11_texture(const device_t &device, void *shared_handle, bool bgra, image_t &image);

  result_e sync_object_from_d3d11_fence(const device_t &device, void *fence_handle, sync_object_t &sync);

}  // namespace pyrowave

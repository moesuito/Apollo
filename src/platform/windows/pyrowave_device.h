/**
 * @file src/platform/windows/pyrowave_device.h
 * @brief PyroWave encode device for the D3D11 capture path.
 */
#pragma once

#ifdef _WIN32

  // standard includes
  #include <d3d11.h>
  #include <dxgi1_2.h>
  #include <memory>

  // local includes
  #include "src/platform/common.h"
  #include "src/pyrowave_codec.h"

  namespace platf {

    class d3d_pyrowave_encode_device_t: public encode_device_t {
    public:
      /**
       * @brief Bind to an adapter and create the Vulkan device the PyroWave
       *        encoder runs on.
       *
       * @param display_p The display being captured. Retained so the device
       *        outlives individual frames.
       * @param adapter The DXGI adapter the display captures from.
       * @return true on success.
       */
      // Declared with IDXGIAdapter1* rather than adapter_t::pointer because
      // adapter_t is a DXGI-namespace alias declared in display.h, which this
      // header deliberately does not include (it would drag in the whole display
      // layer). IDXGIAdapter1* is the concrete type behind that alias.
      bool init_device(std::shared_ptr<display_t> display_p, IDXGIAdapter1 *adapter);

      /**
       * @brief No-op.
       *
       * All of the per-frame work (importing the D3D11 texture, encoding,
       * packetizing) lives in the session, which is the only party that knows the
       * frame is ready to encode. The device just carries the Vulkan device and
       * sync object across to it.
       */
      int convert(img_t &img) override {
        return 0;
      }

      // Both return by value, not by reference: the session takes ownership, and
      // the handle types are move-only, so the caller must be able to move out of
      // them. A const reference here would silently copy-fall-back to the deleted
      // copy constructor.
      [[nodiscard]] pyrowave::device_t take_device() {
        return std::move(m_device);
      }

      [[nodiscard]] pyrowave::sync_object_t take_sync() {
        return std::move(m_sync);
      }

    private:
      std::shared_ptr<display_t> display;
      pyrowave::device_t m_device;
      pyrowave::sync_object_t m_sync;
    };

  }  // namespace platf

  /**
   * @brief The capture texture of a D3D11 image, and whether it is BGRA.
   *
   * Defined in display_vram.cpp next to img_d3d_t, which is private to that file,
   * so the layout is known in exactly one place. Declared at global scope to match
   * that definition; inside a namespace the two would mangle differently and fail
   * to link.
   *
   * @param img The captured image.
   * @param bgra Set to true when the texture is DXGI_FORMAT_B8G8R8A8_UNORM.
   * @param valid Set to false when the image is not a D3D11 one.
   * @return The borrowed texture pointer, or nullptr when @p valid comes back false.
   */
  ID3D11Texture2D *capture_texture_of(platf::img_t &img, bool &bgra, bool &valid);

#endif  // _WIN32

/**
 * @file src/platform/windows/pyrowave_device.cpp
 * @brief PyroWave encode device for the D3D11 capture path.
 */

// local includes
#include "src/logging.h"
#include "src/platform/windows/pyrowave_device.h"

#ifdef _WIN32

  // standard includes
  #include <cstring>

  // local includes
  // to_utf8() for the adapter name, declared in misc.h.
  #include "src/platform/windows/misc.h"

namespace platf {

  bool d3d_pyrowave_encode_device_t::init_device(std::shared_ptr<display_t> display_p, IDXGIAdapter1 *adapter) {
    this->display = std::move(display_p);

    DXGI_ADAPTER_DESC desc;
    if (FAILED(adapter->GetDesc(&desc))) {
      BOOST_LOG(error) << "PyroWave: failed to query the capture adapter"sv;
      return false;
    }

    // Bind the encoder to the same physical GPU the desktop is captured from.
    // Without the LUID the codec picks the first suitable device, which on a
    // hybrid-graphics laptop (RTX 3060 + Intel UHD here) is the wrong one.
    const auto *luid = reinterpret_cast<const unsigned char *>(&desc.AdapterLuid);

    m_device = pyrowave::device_t::create_for_adapter(luid);
    if (!m_device) {
      BOOST_LOG(error) << "PyroWave: no Vulkan device for adapter "sv << to_utf8(desc.Description);
      return false;
    }

    // A timeline semaphore for the acquire/release handshake. The capture
    // textures carry their own keyed mutex, so the encoder is not racing the
    // capture thread on the texture contents; this exists because the encoder's
    // API requires a sync point to be supplied and signalled.
    m_sync = pyrowave::sync_object_t::create_own(m_device);
    if (!m_sync) {
      BOOST_LOG(error) << "PyroWave: failed to create the sync object"sv;
      return false;
    }

    BOOST_LOG(info) << "PyroWave: bound to adapter "sv << to_utf8(desc.Description);
    return true;
  }

}  // namespace platf

#endif  // _WIN32

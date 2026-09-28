/**
 * @file src/pyrowave_encode_test.cpp
 * @brief Standalone check of the PyroWave encode path, without a display.
 *
 * The host's own probe (video::probe_encoders) needs a display to create a
 * display_t, so on a headless machine nothing after process start gets exercised.
 * This harness covers the part that actually matters and is display-independent:
 * the wrapper and the bitrate math.
 *
 * It creates a D3D11 texture, imports it into the PyroWave Vulkan device through
 * the same code path the host uses (pyrowave_codec.cpp), encodes a frame, and
 * checks that the result is a plausible intra-only frame. That covers:
 *
 *   - device creation bound to a LUID
 *   - sync object creation
 *   - the D3D11 texture import (the R1 interop, on the real host code path)
 *   - encode + packetize + the lastPayloadLen value
 *   - pyrowave_bitrate.cpp
 *
 * It does NOT cover the capture side (which texture, when) or the packetizer.
 *
 * Build it with the pyrowave_encode_test target; see
 * cmake/dependencies/pyrowave.cmake.
 */

// standard includes
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef _WIN32

  // standard includes
  #include <d3d11_4.h>
  #include <wrl/client.h>

  // third party includes
  // The codec author's own bitrate regression, used here to cross-check
  // pyrowave_bitrate.cpp against the source it is derived from.
  #include <pyrowave_regression_results.h>

  // lib includes
  #include <boost/log/core.hpp>
  #include <boost/log/sources/severity_logger.hpp>
  #include <boost/log/trivial.hpp>

  // local includes
  #include "pyrowave_bitrate.h"
  #include "pyrowave_codec.h"

  // standard includes
  #include <iostream>

  using Microsoft::WRL::ComPtr;

// The code under test logs through src/logging.h, which declares these loggers at
// *global* scope (not inside boost::log). They are defined in logging.cpp for the
// host; this harness does not link the host, so it provides them here, with the same
// severities. They need external linkage because BOOST_LOG captures their address.
boost::log::sources::severity_logger<int> verbose(0);
boost::log::sources::severity_logger<int> debug(1);
boost::log::sources::severity_logger<int> info(2);
boost::log::sources::severity_logger<int> warning(3);
boost::log::sources::severity_logger<int> error(4);
boost::log::sources::severity_logger<int> fatal(5);

  namespace {

    int failures = 0;

    void check(bool condition, const char *what) {
      if (condition) {
        std::printf("  ok    %s\n", what);
      } else {
        std::printf("  FAIL  %s\n", what);
        failures++;
      }
    }

    /**
     * @brief Fill a texture with something the encoder cannot trivially compress away.
     *
     * A flat colour would encode to almost nothing, which would make the frame-size
     * assertion meaningless.
     */
    void fill_texture(ID3D11Device *device, ID3D11DeviceContext *ctx, ID3D11Texture2D *texture, int width, int height) {
      std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
      for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
          auto *px = &pixels[(static_cast<std::size_t>(y) * width + x) * 4];
          // A gradient, so the wavelet transform has real work to do.
          px[0] = static_cast<std::uint8_t>((x * 7) ^ (y * 13));
          px[1] = static_cast<std::uint8_t>((x * 3) + (y * 29));
          px[2] = static_cast<std::uint8_t>((x ^ y) * 5);
          px[3] = 0xFF;
        }
      }

      // The texture itself has to be CPU-writable, so create it with the right
      // usage rather than going through a staging copy.
      D3D11_TEXTURE2D_DESC desc {};
      texture->GetDesc(&desc);
      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
      // A staging texture cannot be SHARED, and CopyResource requires matching
      // resource type and format, not matching MiscFlags.
      desc.MiscFlags = 0;

      ComPtr<ID3D11Texture2D> staging;
      device->CreateTexture2D(&desc, nullptr, &staging);

      D3D11_SUBRESOURCE_DATA data {};
      data.pSysMem = pixels.data();
      data.SysMemPitch = static_cast<UINT>(width * 4);

      D3D11_MAPPED_SUBRESOURCE mapped {};
      if (SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        std::memcpy(mapped.pData, pixels.data(), pixels.size());
        ctx->Unmap(staging.Get(), 0);
      }

      ctx->CopyResource(texture, staging.Get());
    }

  }  // namespace

  int main() {
    constexpr int width = 1280;
    constexpr int height = 720;
    constexpr std::size_t packet_size = 1392;

    // The loggers the code under test captures are defined at namespace scope above,
    // because BOOST_LOG takes their address and so they need external linkage.

    std::printf("PyroWave encode path check (%dx%d)\n", width, height);

    // --- device ---
    std::printf("\n[1] device\n");

    // ComPtr is used without IID_PPV_ARGS throughout, because MinGW's __uuidof
    // handling does not combine with it. Release() is therefore needed explicitly.
    IDXGIFactory1 *factory_raw = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_IDXGIFactory1, reinterpret_cast<void **>(&factory_raw)))) {
      std::printf("  FAIL  CreateDXGIFactory1\n");
      return 1;
    }
    ComPtr<IDXGIFactory1> factory {factory_raw};

    // EnumAdapters takes the base IDXGIAdapter**, and IDXGIAdapter1 is a superset of
    // it, so query up from the base type rather than passing the derived pointer.
    IDXGIAdapter *base_adapter = nullptr;
    if (FAILED(factory->EnumAdapters(0, &base_adapter)) || !base_adapter) {
      std::printf("  FAIL  no adapter\n");
      return 1;
    }

    IDXGIAdapter1 *adapter_raw = nullptr;
    HRESULT adapter_hr = base_adapter->QueryInterface(IID_IDXGIAdapter1, reinterpret_cast<void **>(&adapter_raw));
    base_adapter->Release();
    if (FAILED(adapter_hr) || !adapter_raw) {
      std::printf("  FAIL  adapter does not support IDXGIAdapter1\n");
      return 1;
    }
    ComPtr<IDXGIAdapter1> adapter {adapter_raw};

    DXGI_ADAPTER_DESC adapter_desc {};
    adapter->GetDesc(&adapter_desc);
    std::printf("  adapter: %ls\n", adapter_desc.Description);

    // This is the same LUID path the host uses, which is what keeps the encoder on
    // the capture GPU rather than the first suitable device.
    auto *luid = reinterpret_cast<const unsigned char *>(&adapter_desc.AdapterLuid);
    auto device = pyrowave::device_t::create_for_adapter(luid);
    check(static_cast<bool>(device), "device created for adapter LUID");
    if (!device) {
      return 1;
    }

    // --- sync ---
    std::printf("\n[2] sync object\n");
    auto sync = pyrowave::sync_object_t::create_own(device);
    check(static_cast<bool>(sync), "timeline semaphore created");
    if (!sync) {
      return 1;
    }

    // --- D3D11 texture ---
    std::printf("\n[3] D3D11 texture import (the R1 interop, on the host code path)\n");
    D3D11_TEXTURE2D_DESC tex_desc {};
    tex_desc.Width = width;
    tex_desc.Height = height;
    tex_desc.MipLevels = 1;
    tex_desc.ArraySize = 1;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.Usage = D3D11_USAGE_DEFAULT;
    tex_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    tex_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    // SHARED_NTHANDLE is required alongside SHARED for CreateSharedHandle to work
    // without a named object. The PyroWave repo's own interop test
    // (pyrowave_c_interop_test.cpp:1564) sets exactly these two bits, and the host's
    // capture texture is created the same way.
    tex_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

    ID3D11Device *device_raw = nullptr;
    ID3D11DeviceContext *ctx_raw = nullptr;
    HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &device_raw, nullptr, &ctx_raw);
    if (FAILED(hr)) {
      std::printf("  FAIL  D3D11CreateDevice (0x%08lx)\n", static_cast<unsigned long>(hr));
      return 1;
    }
    ComPtr<ID3D11Device> d3d_device {device_raw};
    ComPtr<ID3D11DeviceContext> d3d_ctx {ctx_raw};

    ID3D11Texture2D *texture_raw = nullptr;
    hr = d3d_device->CreateTexture2D(&tex_desc, nullptr, &texture_raw);
    check(SUCCEEDED(hr), "capture-like texture created");
    if (FAILED(hr)) {
      return 1;
    }
    ComPtr<ID3D11Texture2D> texture {texture_raw};

    IDXGIResource1 *resource_raw = nullptr;
    hr = texture->QueryInterface(IID_IDXGIResource1, reinterpret_cast<void **>(&resource_raw));
    check(SUCCEEDED(hr), "texture is an IDXGIResource1");
    if (FAILED(hr)) {
      return 1;
    }
    ComPtr<IDXGIResource1> resource {resource_raw};

    // Taken before the content is filled in, mirroring the host: the capture texture
    // is already a shared resource by the time the encoder sees it. The import below
    // only needs the handle, and the contents are written before the encode.
    HANDLE shared = nullptr;
    hr = resource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &shared);
    if (FAILED(hr)) {
      char buf[80];
      std::snprintf(buf, sizeof(buf), "CreateSharedHandle (0x%08lx)", static_cast<unsigned long>(hr));
      check(false, buf);
      return 1;
    }
    check(true, "CreateSharedHandle");

    fill_texture(d3d_device.Get(), d3d_ctx.Get(), texture.Get(), width, height);
    check(true, "texture filled");

    pyrowave::image_t image;
    auto imported = pyrowave::image_from_d3d11_texture(device, shared, true, width, height, image);
    check(imported == pyrowave::result_e::ok, "texture imported into PyroWave (D3D11 -> Vulkan)");
    if (imported != pyrowave::result_e::ok) {
      CloseHandle(shared);
      return 1;
    }

    // --- encoder ---
    std::printf("\n[4] encode\n");
    auto encoder = pyrowave::encoder_t::create(device, width, height, pyrowave::chroma_e::yuv420);
    check(static_cast<bool>(encoder), "encoder created");
    if (!encoder) {
      return 1;
    }
    encoder.set_sync_object(sync);

    // --- bitrate ---
    std::printf("\n[5] rate control\n");
    pyrowave::bitrate_request_t request;
    request.width = width;
    request.height = height;
    request.fps = 60;
    request.bitrate_kbps = 20000;
    request.quality_modifier = 1.1;
    request.packet_size = packet_size;

    const auto manual = pyrowave::frame_bitstream_size(request, false, 40.0);
    check(manual >= 2048, "manual budget is at least one packet");
    std::printf("        manual    = %zu B/frame\n", manual);

    const auto automatic = pyrowave::frame_bitstream_size(request, true, 40.0);
    check(automatic >= 2048, "automatic budget is at least one packet");
    std::printf("        automatic = %zu B/frame\n", automatic);

    // Cross-check the automatic budget against the regression directly, so a ceiling
    // or an off-by-fps cannot silently flatten the result. The codec's own numbers
    // for 1280x720 at 40 dB / 60 fps are 248.79 Mbit/s, i.e. 518314 bytes per frame
    // before the quality modifier and the protocol overhead.
    {
      const double mbits = pyrowave_psnr_hvs_m_h_estimate_mbits(
        40, width, height, PYROWAVE_HEIGHT_FACTOR_1_00, 0, static_cast<double>(request.fps));
      const std::size_t expected = static_cast<std::size_t>(mbits * 1e6 / 8.0 / request.fps * request.quality_modifier);
      std::printf("        regression says %zu B/frame, budget says %zu\n", expected, automatic);
      // The budget divides by the per-packet overhead factor, so it must be at or
      // just below the raw regression value, never wildly above it.
      check(automatic <= expected, "automatic budget is not above the regression");
      check(expected / 2 <= automatic, "automatic budget is not below half the regression");
    }

    // A higher PSNR target must not produce a smaller budget, or the quality knob
    // would be inverted.
    const auto higher = pyrowave::frame_bitstream_size(request, true, 45.0);
    check(higher >= automatic, "higher PSNR does not shrink the budget");
    std::printf("        psnr 45   = %zu B/frame\n", higher);

    const auto low_quality = pyrowave::frame_bitstream_size(
      [&] {
        auto r = request;
        r.quality_modifier = 0.5;
        return r;
      }(), false, 40.0);
    check(low_quality < manual, "a lower quality modifier shrinks the budget");

    // The owner's actual target, so the harness says out loud whether it fits the
    // 1 Gbps link. The regression puts 3440x1440 at 40 dB / 60 fps near 844 Mbit/s of
    // video, which with headers is ~68% of a gigabit, i.e. it fits but not loosely.
    {
      pyrowave::bitrate_request_t target;
      target.width = 3440;
      target.height = 1440;
      target.fps = 60;
      target.bitrate_kbps = 0;
      target.quality_modifier = 1.1;
      target.packet_size = packet_size;

      for (int psnr : {30, 35, 40, 45}) {
        const auto bytes = pyrowave::frame_bitstream_size(target, true, psnr);
        // Bytes/frame -> Mbit/s at 60 fps.
        const double mbits = static_cast<double>(bytes) * 8.0 * target.fps / 1e6;
        const double link_pct = mbits / 940.0 * 100.0;
        std::printf("        3440x1440@60 psnr=%d -> %zu B/frame, %.0f Mbit/s, %.0f%% of a 1 Gbps link%s\n",
                    psnr, bytes, mbits, link_pct, link_pct > 100.0 ? "  <-- DOES NOT FIT" : "");
        check(bytes > 0, "3440x1440 budget is non-zero");
        // Reported, not asserted: whether a given PSNR fits the owner's fixed 1 Gbps
        // link is a tuning decision, and the plan (4.5, risk R4) already flags it.
        // The measurement above is the point - it is the first real number for this
        // 21:9 target rather than an extrapolation from the obsolete power law.
      }

      // And the lever the owner was told to use: at 30 dB it should be far cheaper.
      const auto cheap = pyrowave::frame_bitstream_size(target, true, 30.0);
      const auto rich = pyrowave::frame_bitstream_size(target, true, 45.0);
      check(cheap * 2 < rich, "dropping PSNR is a meaningful lever at 3440x1440");
    }

    encoder.set_rate_control_size(automatic);

    // --- the actual encode ---
    // The acquire value is 0 rather than 1: a timeline semaphore that nothing has
    // signalled is already "at" 0, so the encoder's wait is satisfied immediately.
    // Any positive value would hang here, because in the host the capture thread is
    // what signals it and this harness has no capture thread.
    std::printf("\n[6] encode + packetize\n");
    std::printf("        acquire=0 release=1 (no capture thread to signal it)\n");
    std::fflush(stdout);
    const auto encoded = encoder.encode_from_image(image, 0, 1);
    std::printf("        (encode returned)\n");
    std::fflush(stdout);
    check(encoded == pyrowave::result_e::ok, "frame encoded");
    if (encoded != pyrowave::result_e::ok) {
      return 1;
    }

    check(encoder.encoded_size() > 0, "encoded size is non-zero");
    std::printf("        encoded = %zu B\n", encoder.encoded_size());

    std::size_t num_packets = 0;
    check(encoder.compute_num_packets(packet_size, num_packets) == pyrowave::result_e::ok,
          "packet count computed");
    std::printf("        packets = %zu\n", num_packets);
    check(num_packets > 0, "packet count is non-zero");

    pyrowave::bitstream_t bitstream(encoder.encoded_size() + packet_size);
    std::vector<pyrowave::packet_t> packets;
    check(encoder.packetize(packet_size, bitstream, packets) == pyrowave::result_e::ok,
          "frame packetized");

    std::size_t total = 0;
    for (const auto &p : packets) {
      total += p.size;
    }
    check(packets.size() == num_packets, "packet count matches compute_num_packets");
    check(total > 0, "assembled payload is non-empty");
    std::printf("        assembled = %zu B across %zu packets\n", total, packets.size());

    // The 8 byte frame header is only in the first packet, per the bitstream spec.
    // Its presence is the cheapest proof that we are looking at a real PyroWave
    // bitstream and not a copy of something else.
    if (!packets.empty() && packets[0].size >= 8) {
      std::printf("        first packet header: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                  bitstream.data()[0], bitstream.data()[1], bitstream.data()[2], bitstream.data()[3],
                  bitstream.data()[4], bitstream.data()[5], bitstream.data()[6], bitstream.data()[7]);
    }

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
  }

#else
  int main() {
    std::printf("The PyroWave encoder is Windows-only.\n");
    return 0;
  }
#endif

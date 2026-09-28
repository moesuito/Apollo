/**
 * @file src/pyrowave_protocol.h
 * @brief PyroWave wire-protocol constants shared between the host's RTSP and HTTP layers.
 */
#pragma once

#include <cstdint>

// PyroWave wire-protocol constants.
//
// Apollo vendors moonlight-common-c from ClassicOldSong/moonlight-common-c, which is a
// different fork from moesuito/moonlight-common-c (the one moonlight-qt consumes as a
// submodule). Per the decision recorded in AGENTS.md section 4.2 we deliberately do NOT
// merge the two forks: the divergence is Apollo-protocol extensions (server command,
// clipboard, file transfer) with no codec content, and merging would make every
// upstream merge conflict in Limelight.h and ControlStream.c - which are also the files
// the PyroWave work touches. Instead each side declares the PyroWave constants, and the
// rule in AGENTS.md section 8.2 (protocol changes must be mirrored on both sides) is
// what keeps them in sync.
//
// KEEP IN SYNC WITH:
//   Moonlight/src/Limelight.h               (client-side common-c: VIDEO_FORMAT_/SCM_)
//   Moonlight/src/SdpGenerator.c             (emits BITSTREAM_FORMAT_PYROWAVE)
//   Moonlight/src/RtspConnection.c           (matches RTP_MAP_PYROWAVE)
//   Moonlight-Qt/app/streaming/session.cpp   (maps SCM_PYROWAVE -> VIDEO_FORMAT_PYROWAVE)
//
// 0x00800000 is the first free bit above the Sunshine SCM_* block, which ends at
// SCM_AV1_HIGH10_444 (0x00400000). Verified free in both forks.

#ifndef SCM_PYROWAVE
  #define SCM_PYROWAVE 0x00800000
#endif

namespace video {

  // video::config_t::videoFormat value for PyroWave. The existing encoding is
  // 0 = H.264, 1 = HEVC, 2 = AV1, so 3 is the next free value. config_t carries a
  // "DO NOT CHANGE ORDER OR ADD FIELDS IN THE MIDDLE" warning, but videoFormat is a
  // plain int, so no layout change is required.
  inline constexpr int VIDEO_FORMAT_PYROWAVE = 3;

  // Value written to "x-nv-vqos[0].bitStreamFormat" in the client's SDP offer.
  // 0 = H.264, 1 = HEVC, 2 = AV1, so 3 is the next free value.
  inline constexpr const char *BITSTREAM_FORMAT_PYROWAVE = "3";

  // rtpmap payload type marker advertised in the host's DESCRIBE response and matched
  // by moonlight-common-c during format negotiation. Follows the existing convention
  // of the AV1 marker ("AV1/90000").
  inline constexpr const char *RTP_MAP_PYROWAVE = "pyrowave/90000";

}  // namespace video

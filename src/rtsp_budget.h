/** @brief RTSP bitrate selection and codec-specific bandwidth reservation. */
#pragma once

#include <algorithm>
#include <cstdint>

namespace rtsp_stream {
  inline int64_t select_bitrate(int64_t configured_kbps, int maximum_kbps) {
    return configured_kbps ? configured_kbps : maximum_kbps;
  }

  inline int64_t cap_bitrate(int64_t selected_kbps, int host_max_kbps) {
    return host_max_kbps > 0 ? std::min(selected_kbps, int64_t(host_max_kbps)) : selected_kbps;
  }

  // Pyrowave needs a total video UDP payload budget: its actual frame cost
  // already charges padded RTP, FEC and encryption. Preserve the conventional
  // encoder calculation, including its float division and integer truncation.
  inline int64_t video_bitrate(int64_t negotiated_kbps, int fec_percentage, int audio_channels, bool high_quality_audio, bool pyrowave) {
    if (!pyrowave && fec_percentage <= 80) {
      negotiated_kbps /= 100.f / (100 - fec_percentage);
    }
    const auto audio_kbps = (high_quality_audio ? 256 : 96) * audio_channels;
    negotiated_kbps -= std::min(int64_t(audio_kbps), negotiated_kbps / 5);
    negotiated_kbps -= std::min(int64_t(500), negotiated_kbps / 10);
    return negotiated_kbps;
  }
}  // namespace rtsp_stream

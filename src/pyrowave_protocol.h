/** @brief Version 1 experimental Pyrowave negotiation and full-frame wire contract. */
#pragma once

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace pyrowave {
  inline constexpr std::string_view pin = "89f7e47d4abbf650c91fae766728af866c5e32a0";
  inline constexpr int video_format = 3;
  inline constexpr uint16_t version = 1, header_size = 32;
  inline constexpr size_t packet_boundary = 1200, max_packets = 1024, max_frame_size = 1024 * 1024;
  inline constexpr size_t short_header_size = 8, nv_video_header_size = 16, rtp_header_size = 16;
  inline constexpr size_t encryption_prefix_size = 32;

  struct Dimensions {
    int width = 0, height = 0;

    bool supported() const {
      return (width == 1920 && height == 1080) || (width == 3840 && height == 2160);
    }
  };

  inline bool supported_capture(Dimensions source) {
    return source.width >= 1920 && source.width <= 3840 && source.height >= 1080 && source.height <= 2160 &&
           int64_t(source.width) * 9 == int64_t(source.height) * 16;
  }

  inline bool parse_integer(std::string_view value, int &out) {
    if (value.empty()) {
      return false;
    }
    auto result = std::from_chars(value.data(), value.data() + value.size(), out);
    return result.ec == std::errc {} && result.ptr == value.data() + value.size();
  }

  struct Selection {
    int format = 0;
    std::string_view capability_version, capability_pin;
    int width = 0, height = 0, fps = 0, encoding_fps = 0;
    int dynamic_range = 0, chroma = 0, csc = 0, slices = 0, intra_refresh = 0;
    bool input_only = false;
  };

  // Empty result means conventional or accepted Pyrowave. Call before session allocation.
  inline std::string_view validate_selection(const Selection &s, bool enabled) {
    if (s.format != video_format) {
      if (!s.capability_version.empty() || !s.capability_pin.empty()) {
        return "Pyrowave capability requires bitStreamFormat 3";
      }
      return s.format >= 0 && s.format <= 2 ? "" : "Unknown video format";
    }
    if (!enabled) {
      return "Experimental Pyrowave host is disabled (build, runtime opt-in and KMS required)";
    }
    if (s.capability_version != "1" || s.capability_pin != pin) {
      return "Pyrowave requires matching explicit version and codec pin";
    }
    if (!Dimensions {s.width, s.height}.supported() || s.fps != 60 || s.encoding_fps != 60000 || s.dynamic_range != 0 || s.chroma != 0 || s.csc != 3 || s.slices != 1 || s.intra_refresh != 0 || s.input_only) {
      return "Pyrowave v1 requires 1920x1080 or 3840x2160 at 60 fps, SDR full BT.709 4:2:0, one slice, no intra refresh";
    }
    return {};
  }

  struct Transport {
    int packet_size = 0, fec_percentage = 0, min_parity = 0;
    int bitrate_kbps = 0;  // Adjusted video budget, including our RTP/FEC/encryption overhead.
    bool encrypted = false;
  };

  struct FrameCost {
    size_t data_shards = 0, wire_packets = 0, wire_bytes = 0;
    bool fits = false;
  };

  struct Limits {
    Transport transport;
    size_t frame_bytes = 0, wire_bytes_per_frame = 0;

    FrameCost cost(size_t bytes) const {
      FrameCost result;
      if (!bytes || bytes > max_frame_size) {
        return result;
      }
      const size_t payload = transport.packet_size - nv_video_header_size;
      const size_t block = transport.packet_size + rtp_header_size;
      const size_t data = (bytes + short_header_size + payload - 1) / payload;
      const size_t per_block = 255 * 100 / (100 + transport.fec_percentage);
      const size_t blocks = (data + per_block - 1) / per_block;
      if (!blocks || blocks > 4) {
        return result;
      }
      // Match videoBroadcastThread's byte division followed by block alignment.
      const size_t expanded = bytes + short_header_size + data * (nv_video_header_size + rtp_header_size);
      const size_t aligned = (expanded / blocks + block - 1) / block;
      if ((blocks - 1) * aligned * block >= expanded) {
        return result;
      }
      for (size_t i = 0; i < blocks; ++i) {
        const size_t shards = i + 1 == blocks ? (expanded - i * aligned * block + block - 1) / block : aligned;
        const size_t parity = std::max((shards * transport.fec_percentage + 99) / 100, size_t(transport.min_parity));
        // RS is GF(256); also avoids the broadcaster's oversized-frame FEC fallback.
        if (shards + parity > 255) {
          return result;
        }
        result.data_shards += shards;
        result.wire_packets += shards + parity;
      }
      result.wire_bytes = result.wire_packets * (block + (transport.encrypted ? encryption_prefix_size : 0));
      result.fits = result.wire_bytes <= wire_bytes_per_frame;
      return result;
    }
  };

  inline Limits make_limits(const Transport &t) {
    if (t.packet_size < 1024 || t.packet_size > 1392 || t.fec_percentage < 1 || t.fec_percentage > 80 || t.min_parity < 0 || t.min_parity > 2 || t.bitrate_kbps < 10000 || t.bitrate_kbps > 200000) {
      throw std::runtime_error("Pyrowave transport requires packetSize 1024..1392, FEC 1..80%, minimum parity 0..2, adjusted bitrate 10000..200000 Kbps");
    }
    Limits limits {t, 0, uint64_t(t.bitrate_kbps) * 1000 / 8 / 60};
    // Costs are monotone in each shard interval. FEC alignment can exceed an RS
    // block limit at a boundary, so stop at the first rejected shard interval.
    const size_t payload = t.packet_size - nv_video_header_size;
    for (size_t bytes = payload - short_header_size; bytes <= max_frame_size; bytes += payload) {
      if (!limits.cost(bytes).fits) {
        break;
      }
      limits.frame_bytes = bytes;
    }
    if (limits.frame_bytes < header_size + 4 + packet_boundary) {
      throw std::runtime_error("Pyrowave frame budget cannot hold a codec packet");
    }
    return limits;
  }

  struct PacketView {
    const uint8_t *data;
    size_t size;
  };

  inline void put16(std::vector<uint8_t> &out, uint16_t n) {
    out.push_back(n >> 8);
    out.push_back(n);
  }

  inline void put32(std::vector<uint8_t> &out, uint32_t n) {
    put16(out, n >> 16);
    put16(out, n);
  }

  inline std::vector<uint8_t> envelope(uint32_t frame, Dimensions output, const std::vector<PacketView> &packets, const Limits &limits) {
    if (!output.supported()) {
      throw std::runtime_error("Unsupported Pyrowave output dimensions");
    }
    if (!frame || packets.empty() || packets.size() > max_packets) {
      throw std::runtime_error("Invalid Pyrowave frame number or packet count");
    }
    size_t total = header_size;
    for (auto p : packets) {
      if (!p.data || !p.size || p.size > packet_boundary) {
        throw std::runtime_error("Invalid Pyrowave packet boundary");
      }
      total += 4 + p.size;
    }
    if (total > limits.frame_bytes || !limits.cost(total).fits) {
      throw std::runtime_error("Pyrowave frame exceeds negotiated transport/FEC/bandwidth budget");
    }
    std::vector<uint8_t> out;
    out.reserve(total);
    out.insert(out.end(), {'P', 'W', 'R', '1'});
    put16(out, version);
    put16(out, header_size);
    put32(out, frame);
    put32(out, total);
    put16(out, output.width);
    put16(out, output.height);
    put16(out, packets.size());
    put16(out, 1);  // Every frame is independent.
    put32(out, 1);  // sRGB / BT.709 full range / centered 420 / 128/255.
    put32(out, 0);
    for (auto p : packets) {
      put32(out, p.size);
      out.insert(out.end(), p.data, p.data + p.size);
    }
    return out;
  }
}  // namespace pyrowave

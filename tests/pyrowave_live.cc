/** @brief CPU tests for live negotiation, wire limits, framing and shutdown ownership. */
#include "src/platform/linux/pyrowave_encoder_layout.h"
#include "src/pyrowave_lifetime.h"
#include "src/pyrowave_protocol.h"
#include "src/rtsp_budget.h"

#include <array>
#include <atomic>
#include <iostream>
#include <thread>

namespace {
  void require(bool ok, const char *message) {
    if (!ok) {
      throw std::runtime_error(message);
    }
  }

  template<class Fn>
  void rejects(Fn fn) {
    try {
      fn();
    } catch (const std::runtime_error &) {
      return;
    }
    throw std::runtime_error("Invalid input was accepted");
  }

  void negotiation() {
    using namespace pyrowave;
    int parsed = 0;
    require(parse_integer("60000", parsed) && parsed == 60000, "Decimal parsing failed");
    for (auto text : {"", "60fps", "60.0", "+60", "2147483648", "18446744073709551616"}) {
      require(!parse_integer(text, parsed), "Malformed/overflowing session integer accepted");
    }
    Selection standard;
    require(validate_selection(standard, false).empty(), "Default build/client must stay conventional");
    Selection s {3, "2", pin, 1920, 1080, 60, 60000, 0, 0, 3, 1, 0, false};
    require(validate_selection(s, true).empty(), "Matching 1080p selection rejected");
    auto qhd = s;
    qhd.width = 2560;
    qhd.height = 1440;
    require(validate_selection(qhd, true).empty(), "Matching 1440p selection rejected");
    for (int Selection::*field : {&Selection::fps, &Selection::encoding_fps, &Selection::dynamic_range}) {
      auto bad_qhd = qhd;
      ++(bad_qhd.*field);
      require(!validate_selection(bad_qhd, true).empty(), "1440p accepted an unsupported rate or HDR");
    }
    auto uhd = s;
    uhd.width = 3840;
    uhd.height = 2160;
    require(validate_selection(uhd, true).empty(), "Matching 4K selection rejected");
    require(raw_block_count({1920, 1080}) == 3261, "1080p pinned metadata extent differs");
    require(raw_block_count({2560, 1440}) == 5667, "1440p pinned metadata extent differs");
    require(raw_block_count({3840, 2160}) == 12429, "4K pinned metadata extent differs");
    for (auto source : {Dimensions {1920, 1080}, Dimensions {2560, 1440}, Dimensions {3840, 2160}}) {
      require(supported_capture(source), "Supported native/scaled capture rejected");
    }
    for (auto dims : {Dimensions {0, 0}, Dimensions {-3840, -2160}, Dimensions {1920, 2160}, Dimensions {3840, 1080}, Dimensions {2560, 1080}, Dimensions {1920, 1440}, Dimensions {2561, 1440}, Dimensions {2560, 1441}, Dimensions {3841, 2160}, Dimensions {3840, 2161}, Dimensions {7680, 4320}, Dimensions {65536, 65536}}) {
      auto invalid = uhd;
      invalid.width = dims.width;
      invalid.height = dims.height;
      require(!validate_selection(invalid, true).empty(), "Unsupported/mismatched output dimensions accepted");
      rejects([&]() {
        raw_block_count(dims);
      });
    }
    for (auto source : {Dimensions {1280, 720}, Dimensions {3840, 2161}, Dimensions {2560, 1441}, Dimensions {7680, 4320}, Dimensions {-1, 1080}}) {
      require(!supported_capture(source), "Unsupported capture dimensions accepted");
    }
    uhd.dynamic_range = 1;
    require(!validate_selection(uhd, true).empty(), "4K HDR silently accepted as SDR");
    require(!validate_selection(s, false).empty(), "Disabled host accepted Pyrowave");
    for (auto invalid : {"", "0", "1", "3", "02", "2garbage"}) {
      auto bad = s;
      bad.capability_version = invalid;
      require(!validate_selection(bad, true).empty(), "Unsupported version accepted");
    }
    auto bad = s;
    bad.capability_pin = "different";
    require(!validate_selection(bad, true).empty(), "Different codec pin accepted");
    bad = s;
    bad.format = 0;
    require(!validate_selection(bad, true).empty(), "Conventional codec with custom capability accepted");
    for (int Selection::*field : {&Selection::width, &Selection::height, &Selection::fps, &Selection::encoding_fps, &Selection::dynamic_range, &Selection::chroma, &Selection::csc, &Selection::slices, &Selection::intra_refresh}) {
      bad = s;
      ++(bad.*field);
      require(!validate_selection(bad, true).empty(), "Unsupported video parameter accepted");
    }
    bad = s;
    bad.input_only = true;
    require(!validate_selection(bad, true).empty(), "Input-only Pyrowave accepted");
    standard.format = 4;
    require(!validate_selection(standard, true).empty(), "Unknown codec fell back to H264");
  }

  void transport() {
    using namespace pyrowave;
    // 2 data shards + ceil(2*20%) = 1 parity; 1216 bytes per shard.
    const auto limits = make_limits({1200, 20, 0, 100000, false});
    auto c = limits.cost(1200);
    require(c.fits && c.data_shards == 2 && c.wire_packets == 3 && c.wire_bytes == 3648, "RTP/FEC packet accounting differs");
    auto enc = make_limits({1200, 20, 0, 100000, true}).cost(1200);
    require(enc.wire_bytes == 3744, "Encryption prefix missing from wire budget");
    require(make_limits({1200, 20, 2, 100000, true}).cost(32).wire_packets == 3, "Minimum parity is not counted");
    const auto low = make_limits({1200, 20, 0, 10000, true});
    require(low.frame_bytes < limits.frame_bytes && low.cost(low.frame_bytes).fits, "Bitrate budget is not enforced");
    require(low.frame_bytes < max_codec_record_size, "Low bitrate unexpectedly requires a 64 KiB frame budget");
    require(!low.cost(low.frame_bytes + 1200).fits, "Oversized bandwidth frame accepted");
    const auto wide = make_limits({1392, 1, 0, 200000, false});
    require(!wide.cost(max_frame_size).fits, "Frame requiring >4 RS blocks accepted");
    for (const auto &t : {Transport {0, 20, 0, 100000, false}, Transport {1393, 20, 0, 100000, false}, Transport {1200, 0, 0, 100000, false}, Transport {1200, 81, 0, 100000, false}, Transport {1200, 20, 3, 100000, false}, Transport {1200, 20, -1, 100000, false}, Transport {1200, 20, 0, 9999, false}, Transport {1200, 20, 0, 200001, false}}) {
      rejects([&]() {
        make_limits(t);
      });
      require(!Limits {t, max_frame_size, std::numeric_limits<size_t>::max()}.cost(32).fits, "Malformed transport bypassed cost guard");
    }
    // Across supported MTUs/FEC/min-parity, independently count padded RS
    // shards at the same FEC boundaries used by Apollo's broadcaster.
    for (int mtu : {1024, 1200, 1392}) {
      for (int fec : {1, 20, 30, 80}) {
        for (int parity : {0, 2}) {
          const auto l = make_limits({mtu, fec, parity, 200000, true});
          for (size_t bytes = 32; bytes <= l.frame_bytes; bytes += 37) {
            auto cost = l.cost(bytes);
            require(cost.fits && cost.wire_packets <= 1020 && cost.wire_bytes <= l.wire_bytes_per_frame, "Negotiated cap admits invalid transport cost");
          }
        }
      }
    }
  }

  std::vector<pyrowave::PacketView> frame_records(size_t total, const uint8_t *record) {
    using namespace pyrowave;
    const size_t count = (total - header_size + max_codec_record_size + 3) / (max_codec_record_size + 4);
    size_t remaining = total - header_size - count * 4;
    std::vector<PacketView> result;
    for (size_t i = 0; i < count; ++i) {
      const size_t bytes = std::min(max_codec_record_size, remaining - (count - i - 1));
      result.push_back({record, bytes});
      remaining -= bytes;
    }
    require(!remaining, "Frame fixture length differs");
    return result;
  }

  void rtsp_budgets() {
    using namespace rtsp_stream;
    using namespace pyrowave;
    require(select_bitrate(150000, 200000) == 150000 && select_bitrate(0, 150000) == 150000, "RTSP configured/fallback bitrate changed");
    require(cap_bitrate(150000, 100000) == 100000 && cap_bitrate(150000, 200000) == 150000 && cap_bitrate(150000, 0) == 150000, "Host ceiling exceeds request or is ignored");
    require(video_bitrate(150000, 30, 2, true, true) == 148988, "Pyrowave counted FEC before frame cost or lost audio/control reservation");
    require(video_bitrate(150000, 30, 2, false, true) == 149308, "Normal-quality audio reservation differs");
    require(video_bitrate(10000, 30, 8, true, true) == 7500, "Audio/control reservation percentage caps differ");
    // Frozen conventional RTSP calculation, covering float rounding, tiny
    // rates, both quality levels, and the existing >80% FEC exception.
    for (int bitrate : {1, 100, 9999, 10000, 100001, 150000, 200000}) {
      for (int fec : {0, 1, 20, 30, 80, 81, 100}) {
        for (int channels : {2, 6, 8}) {
          for (bool quality : {false, true}) {
            int64_t old = bitrate;
            if (fec <= 80) {
              old /= 100.f / (100 - fec);
            }
            old -= std::min(int64_t((quality ? 256 : 96) * channels), old / 5);
            old -= std::min(int64_t(500), old / 10);
            require(video_bitrate(bitrate, fec, channels, quality, false) == old, "Conventional bitrate behavior changed");
          }
        }
      }
    }
    for (int requested : {150000, 200000}) {
      for (int host_cap : {0, 100000, 200000}) {
        for (bool quality : {false, true}) {
          const auto negotiated = cap_bitrate(select_bitrate(requested, 200000), host_cap);
          const int budget = video_bitrate(negotiated, 30, 2, quality, true);
          const int reserved = (quality ? 512 : 192) + 500;
          for (int mtu : {1024, 1392}) {
            for (bool encrypted : {false, true}) {
              const auto l = make_limits({mtu, 30, 0, budget, encrypted});
              const auto c = l.cost(l.frame_bytes);
              require(c.fits && c.wire_bytes * 8 * 60 + uint64_t(reserved) * 1000 <= uint64_t(negotiated) * 1000, "RTSP video wire cost plus reserves exceeds request/host ceiling");
              require(l.transport.bitrate_kbps == budget && l.wire_bytes_per_frame == uint64_t(budget) * 1000 / 8 / 60, "RTSP budget was reduced again before cost");
              require(l.frame_bytes > make_limits({mtu, 30, 0, int(video_bitrate(negotiated, 30, 2, quality, false)), encrypted}).frame_bytes, "Duplicate-FEC regression did not increase frame allowance");
              std::array<uint8_t, max_codec_record_size> record {};
              for (auto dims : {Dimensions {1920, 1080}, Dimensions {2560, 1440}, Dimensions {3840, 2160}}) {
                const auto frame = envelope(1, dims, frame_records(l.frame_bytes, record.data()), l);
                require(frame.size() == l.frame_bytes && l.cost(frame.size()).fits, "RTSP budget differs by output mode");
                rejects([&]() {
                  envelope(1, dims, frame_records(l.frame_bytes + 1, record.data()), l);
                });
              }
              if (requested == 150000 && host_cap == 0 && quality && mtu == 1392 && !encrypted) {
                require(l.frame_bytes == 232536 && l.codec_target_bytes() == 228408, "150 Mbps FEC30 golden budget differs");
                std::cout << "RTSP 150000 Kbps, stereo HQ, FEC30: wire budget=" << budget << " Kbps, frame=" << l.frame_bytes << ", codec target=" << l.codec_target_bytes() << " bytes\n";
              }
            }
          }
        }
      }
    }
    for (int requested : {0, 9999, 10000}) {
      rejects([&]() {
        make_limits({1392, 30, 0, int(video_bitrate(cap_bitrate(select_bitrate(requested, 0), 0), 30, 2, true, true)), false});
      });
    }
  }

  // Independent byte-size simulation of concat_and_insert, FEC block slicing
  // and fec::encode in stream.cpp. Deliberately derive block count from the
  // expanded byte stream rather than Limits::cost's initial shard count.
  pyrowave::FrameCost broadcaster_cost(const pyrowave::Transport &t, size_t bytes) {
    pyrowave::FrameCost result;
    const size_t packet_bytes = t.packet_size + 16;
    const size_t frame = bytes + 8;
    const size_t inserted = ((frame - 1) / (t.packet_size - 16) + 1) * 32;
    const size_t expanded = frame + inserted;
    const size_t max_block_bytes = (25500 / (100 + t.fec_percentage)) * packet_bytes;
    const size_t blocks = (expanded - 1) / max_block_bytes + 1;
    if (blocks > 4 || bytes > pyrowave::max_frame_size) {
      return result;
    }
    const size_t split = ((expanded / blocks + packet_bytes - 1) / packet_bytes) * packet_bytes;
    size_t remaining = expanded;
    for (size_t i = 0; i < blocks; ++i) {
      const size_t slice = i + 1 == blocks ? remaining : split;
      if (!slice || slice > remaining) {
        return {};
      }
      remaining -= slice;
      const size_t data = (slice - 1) / packet_bytes + 1;
      const size_t parity = std::max((data * t.fec_percentage + 99) / 100, size_t(t.min_parity));
      if (data + parity > 255) {
        return {};
      }
      result.data_shards += data;
      result.wire_packets += data + parity;
    }
    result.wire_bytes = result.wire_packets * (packet_bytes + (t.encrypted ? 32 : 0));
    result.fits = result.wire_bytes <= uint64_t(t.bitrate_kbps) * 1000 / 8 / 60;
    return result;
  }

  void broadcaster_boundaries() {
    using namespace pyrowave;
    size_t nonmonotone = 0;
    bool checked_guard = false;
    size_t eligible_nonmonotone = 0;
    for (int mtu : {1024, 1392}) {
      for (int fec = 1; fec <= 80; ++fec) {
        for (bool encrypted : {false, true}) {
          for (int parity : {0, 2}) {
            const auto l = make_limits({mtu, fec, parity, 200000, encrypted});
            const size_t payload = mtu - 16;
            FrameCost previous;
            size_t previous_bytes = 0;
            for (size_t shard = 1; shard * payload - 8 <= max_frame_size; ++shard) {
              const size_t high = shard * payload - 8;
              // Inspect partial shards and both sides of aligned byte splits,
              // not only the maximal endpoint of each shard interval.
              for (size_t bytes : {std::max(size_t(1), high - payload + 1), high - payload / 2, high - 2, high - 1, high}) {
                const auto expected = broadcaster_cost(l.transport, bytes), actual = l.cost(bytes);
                require(actual.data_shards == expected.data_shards && actual.wire_packets == expected.wire_packets && actual.wire_bytes == expected.wire_bytes && actual.fits == expected.fits, "Production cost differs from broadcaster padding/FEC/encryption");
                if (previous.wire_bytes && actual.wire_bytes && actual.wire_bytes < previous.wire_bytes) {
                  if (!nonmonotone) {
                    std::cout << "Nonmonotonic broadcaster example: MTU=" << mtu << " FEC=" << fec << " parity=" << parity << " encrypted=" << encrypted
                              << " bytes=" << previous_bytes << '/' << bytes << " wire=" << previous.wire_bytes << '/' << actual.wire_bytes << '\n';
                  }
                  ++nonmonotone;
                  if (actual.fits) {
                    ++eligible_nonmonotone;
                  }
                  if (!checked_guard) {
                    // The nonmonotonic boundaries are beyond the 200 Mbps
                    // guard. Even forged scalar limits must not admit them.
                    auto forged = l;
                    forged.wire_bytes_per_frame = std::numeric_limits<size_t>::max();
                    forged.frame_bytes = bytes;
                    require(!forged.cost(previous_bytes).fits && !forged.cost(bytes).fits, "Forged limits bypass negotiated wire budget");
                    std::array<uint8_t, max_codec_record_size> record {};
                    for (auto dims : {Dimensions {1920, 1080}, Dimensions {2560, 1440}, Dimensions {3840, 2160}}) {
                      rejects([&]() {
                        envelope(1, dims, frame_records(previous_bytes, record.data()), forged);
                      });
                      rejects([&]() {
                        envelope(1, dims, frame_records(bytes, record.data()), forged);
                      });
                    }
                    checked_guard = true;
                  }
                }
                previous = actual;
                previous_bytes = bytes;
                if (bytes <= l.frame_bytes) {
                  require(actual.fits, "Conservative cap admits a transport hole");
                }
              }
            }
            require(!l.cost(0).fits && !l.cost(max_frame_size + 1).fits, "Empty/absolute-size guard changed");
          }
        }
      }
    }
    require(nonmonotone && checked_guard && !eligible_nonmonotone, "Cross-check missed nonmonotonic FEC cost or wire guard");
    std::cout << "Broadcaster cross-check: all FEC1..80, MTU1024/1392, plain/encrypted, parity0/2; nonmonotonic samples=" << nonmonotone << ", within 200 Mbps=" << eligible_nonmonotone << '\n';
  }

  void framing() {
    using namespace pyrowave;
    const Dimensions hd {1920, 1080}, qhd {2560, 1440}, uhd {3840, 2160};
    auto limits = make_limits({1200, 20, 0, 100000, false});
    const uint8_t a[] {0x01, 0x02, 0x03}, b[] {0xff, 0x80};
    auto frame = envelope(0x01020304, hd, {{a, 3}, {b, 2}}, limits);
    const std::vector<uint8_t> expected {
      'P',
      'W',
      'R',
      '2',
      0,
      2,
      0,
      32,
      1,
      2,
      3,
      4,
      0,
      0,
      0,
      45,
      7,
      128,
      4,
      56,
      0,
      2,
      0,
      1,
      0,
      0,
      0,
      1,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      3,
      1,
      2,
      3,
      0,
      0,
      0,
      2,
      0xff,
      0x80
    };
    require(frame == expected, "Wire contract differs from fixed BE golden frame");
    auto expected_qhd = expected;
    expected_qhd[16] = 0x0a;
    expected_qhd[17] = 0x00;
    expected_qhd[18] = 0x05;
    expected_qhd[19] = 0xa0;
    auto frame_qhd = envelope(0x01020304, qhd, {{a, 3}, {b, 2}}, limits);
    require(frame_qhd == expected_qhd, "1440p dimensions differ from BE golden envelope");
    require(limits.cost(frame.size()).wire_bytes == limits.cost(frame_qhd.size()).wire_bytes, "1440p increased transport allowance");
    auto expected_4k = expected;
    expected_4k[16] = 0x0f;
    expected_4k[17] = 0x00;
    expected_4k[18] = 0x08;
    expected_4k[19] = 0x70;
    auto frame_4k = envelope(0x01020304, uhd, {{a, 3}, {b, 2}}, limits);
    require(frame_4k == expected_4k, "4K dimensions differ from BE golden envelope");
    require(limits.cost(frame.size()).wire_bytes == limits.cost(frame_4k.size()).wire_bytes, "4K increased transport allowance");
    for (auto dims : {Dimensions {1920, 2160}, Dimensions {3840, 1080}, Dimensions {2560, 1080}, Dimensions {1920, 1440}, Dimensions {2561, 1440}, Dimensions {2560, 1441}, Dimensions {3841, 2160}, Dimensions {65536, 65536}}) {
      rejects([&]() {
        envelope(1, dims, {{a, 3}}, limits);
      });
    }
    rejects([&]() {
      envelope(0, hd, {{a, 3}}, limits);
    });
    rejects([&]() {
      envelope(1, hd, {}, limits);
    });
    rejects([&]() {
      envelope(1, hd, {{nullptr, 3}}, limits);
    });
    rejects([&]() {
      envelope(1, hd, {{a, 0}}, limits);
    });
    std::vector<uint8_t> large_record(64 * 1024 + 1, 0xa5);
    const auto wide = make_limits({1392, 1, 0, 200000, false});
    auto complete = envelope(1, hd, {{large_record.data(), 64 * 1024}}, wide);
    require(complete.size() == header_size + 4 + 64 * 1024 && complete[32] == 0 && complete[33] == 1 && complete[34] == 0 && complete[35] == 0, "64 KiB codec record or BE length rejected");
    require(std::equal(complete.begin() + 36, complete.end(), large_record.begin()), "Codec record bytes changed");
    rejects([&]() {
      envelope(1, hd, {{large_record.data(), large_record.size()}}, wide);
    });
    rejects([&]() {
      envelope(1, hd, {{large_record.data(), 64 * 1024}}, make_limits({1024, 80, 2, 10000, true}));
    });
    std::vector<PacketView> too_many(max_packets + 1, {a, 3});
    rejects([&]() {
      envelope(1, hd, too_many, limits);
    });
    require(!envelope(1, hd, std::vector<PacketView>(1024, {a, 1}), wide).empty(), "Maximum record count rejected");
    auto absolute = wide;
    absolute.frame_bytes = max_frame_size + 1;
    absolute.wire_bytes_per_frame = std::numeric_limits<size_t>::max();
    rejects([&]() {
      envelope(1, hd, std::vector<PacketView>(16, {large_record.data(), 64 * 1024}), absolute);
    });
    std::array<uint8_t, 1200> packet {};
    for (const auto &bounded : {limits, make_limits({1024, 80, 2, 10000, true}), make_limits({1392, 1, 0, 200000, false})}) {
      std::vector<PacketView> within((bounded.frame_bytes - header_size) / (4 + packet.size()), {packet.data(), packet.size()});
      auto too_big = within;
      too_big.push_back({packet.data(), packet.size()});
      for (auto output : {hd, qhd, uhd}) {
        auto accepted = envelope(1, output, within, bounded);
        require(accepted.size() <= bounded.frame_bytes && bounded.cost(accepted.size()).fits, "Output dimensions bypassed bandwidth/FEC bounds");
        rejects([&]() {
          envelope(1, output, too_big, bounded);
        });
      }
    }
  }

  void shutdown_and_reconnect() {
    using namespace pyrowave;
    using namespace std::chrono_literals;
    CaptureGate gate;
    auto conventional = gate.acquire(false);
    auto second = gate.acquire(false);
    require(conventional && second && !gate.acquire(true), "Pyrowave overlapped conventional capture");
    conventional.reset();
    second.reset();
    auto custom = gate.acquire(true);
    require(custom && !gate.acquire(true) && !gate.acquire(false), "Exclusive capture not enforced");
    auto window = std::make_shared<FrameWindow>();
    auto queued = window->acquire(), in_flight = window->acquire();
    require(queued && in_flight && !window->acquire(), "More than two pending frames permitted");
    window->close();
    require(!window->acquire(), "Frame accepted after shutdown");
    queued.reset();  // Queued packets discarded by session join.
    require(!window->wait_drained(0ms), "Session pointer released during broadcast");
    std::thread sender([packet = std::move(in_flight)]() mutable {
      packet.reset();
    });
    const bool drained = window->wait_drained(1s);
    sender.join();
    require(drained, "Shutdown did not drain broadcaster ticket");
    custom.reset();
    require(bool(gate.acquire(true)), "Reconnect capture lease was not released");
    auto reconnect = std::make_shared<FrameWindow>();
    require(bool(reconnect->acquire()), "Old shutdown state leaked into reconnect");
  }
}  // namespace

int main() {
  try {
    negotiation();
    transport();
    rtsp_budgets();
    broadcaster_boundaries();
    framing();
    shutdown_and_reconnect();
    std::cout << "Pyrowave live negotiation, transport, envelope, shutdown/reconnect checks passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}

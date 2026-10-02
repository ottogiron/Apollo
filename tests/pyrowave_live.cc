/** @brief CPU tests for live negotiation, wire limits, framing and shutdown ownership. */
#include "src/platform/linux/pyrowave_encoder_layout.h"
#include "src/pyrowave_lifetime.h"
#include "src/pyrowave_protocol.h"

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
    }
    // Across supported MTUs/FEC/min-parity, independently count padded RS
    // shards at the same FEC boundaries used by Apollo's broadcaster.
    for (int mtu : {1024, 1200, 1392}) {
      for (int fec : {1, 20, 80}) {
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
    framing();
    shutdown_and_reconnect();
    std::cout << "Pyrowave live negotiation, transport, envelope, shutdown/reconnect checks passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}

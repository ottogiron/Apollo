/** @brief Conventional WLR/NVENC RAM contract, without compositor or GPU I/O. */
#if defined(__linux__) && defined(SUNSHINE_BUILD_WAYLAND)
  #include "src/config.h"
  #include "src/platform/linux/wayland.h"
  #include "src/platform/linux/wlgrab_test.h"
  #include "src/video.h"
  #include "tests/tests_common.h"

extern "C" {
  #include <libavutil/imgutils.h>
}

namespace platf {
  std::shared_ptr<display_t> wl_display(mem_type_e, const std::string &, const video::config_t &);
}

namespace video {
  // Exercise the production converter/session, with injected codec I/O for
  // inspecting the exact YUV bytes that would be uploaded to NVENC.
  std::unique_ptr<platf::encode_device_t> make_encode_device(platf::display_t &, const encoder_t &, const config_t &);
  std::unique_ptr<encode_session_t> make_encode_session(platf::display_t *, const encoder_t &, const config_t &, int, int, std::unique_ptr<platf::encode_device_t>);
  int encode(int64_t, encode_session_t &, safe::mail_raw_t::queue_t<packet_t> &, void *, std::optional<std::chrono::steady_clock::time_point>);
}  // namespace video

namespace {
  bool inject_import = false;
  int readback_width, readback_height;
  unsigned readback_calls;
  std::array<uint8_t, 4> readback_color;
  bool inject_codec_sink = false;
  std::vector<uint8_t> converted_pixels;
}  // namespace

extern "C" int __real_avcodec_open2(AVCodecContext *, const AVCodec *, AVDictionary **);
extern "C" int __real_avcodec_send_frame(AVCodecContext *, const AVFrame *);
extern "C" int __real_avcodec_receive_packet(AVCodecContext *, AVPacket *);

extern "C" int __wrap_avcodec_open2(AVCodecContext *context, const AVCodec *codec, AVDictionary **options) {
  if (!inject_codec_sink) {
    return __real_avcodec_open2(context, codec, options);
  }
  av_dict_free(options);
  return 0;
}

extern "C" int __wrap_avcodec_send_frame(AVCodecContext *context, const AVFrame *frame) {
  if (!inject_codec_sink) {
    return __real_avcodec_send_frame(context, frame);
  }
  if (frame) {
    auto format = static_cast<AVPixelFormat>(frame->format);
    auto size = av_image_get_buffer_size(format, frame->width, frame->height, 1);
    if (size < 0) {
      return size;
    }
    converted_pixels.resize(size);
    return av_image_copy_to_buffer(converted_pixels.data(), size, frame->data, frame->linesize, format, frame->width, frame->height, 1) < 0 ? -1 : 0;
  }
  return 0;
}

extern "C" int __wrap_avcodec_receive_packet(AVCodecContext *context, AVPacket *packet) {
  return inject_codec_sink ? AVERROR(EAGAIN) : __real_avcodec_receive_packet(context, packet);
}

extern "C" std::optional<egl::rgb_t> __real__ZN3egl13import_sourceEPvRKNS_20surface_descriptor_tE(void *, const egl::surface_descriptor_t &);

extern "C" std::optional<egl::rgb_t> __wrap__ZN3egl13import_sourceEPvRKNS_20surface_descriptor_tE(void *display, const egl::surface_descriptor_t &source) {
  if (!inject_import) {
    return __real__ZN3egl13import_sourceEPvRKNS_20surface_descriptor_tE(display, source);
  }
  EXPECT_EQ(source.width, readback_width);
  EXPECT_EQ(source.height, readback_height);
  egl::rgb_t image;
  image->tex = gl::tex_t::make(1);
  return image;
}

namespace {
  class WlrRamContract: public testing::Test {
  protected:
    void SetUp() override {
      saved_threads = config::video.min_threads;
      config::video.min_threads = 2;
      saved_gl = gl::ctx;
      inject_import = true;
      readback_calls = 0;
      gl::ctx.GenTextures = [](GLsizei count, GLuint *textures) {
        std::fill_n(textures, count, 42);
      };
      gl::ctx.DeleteTextures = [](GLsizei, const GLuint *) {
      };
      gl::ctx.BindTexture = [](GLenum, GLuint) {
      };
      gl::ctx.TexParameteri = [](GLenum, GLenum, GLint) {
      };
      gl::ctx.TexParameterfv = [](GLenum, GLenum, const GLfloat *) {
      };
      gl::ctx.GetTexLevelParameteriv = [](GLenum, GLint, GLenum name, GLint *value) {
        *value = name == GL_TEXTURE_WIDTH ? readback_width : readback_height;
      };
      gl::ctx.GetTextureSubImage = [](GLuint, GLint, GLint x, GLint y, GLint z, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, GLsizei size, void *pixels) {
        ++readback_calls;
        EXPECT_EQ(x, 0);
        EXPECT_EQ(y, 0);
        EXPECT_EQ(z, 0);
        EXPECT_EQ(depth, 1);
        EXPECT_EQ(width, readback_width);
        EXPECT_EQ(height, readback_height);
        EXPECT_EQ(format, GL_BGRA);
        EXPECT_EQ(type, GL_UNSIGNED_BYTE);
        ASSERT_EQ(size, readback_width * readback_height * 4);
        ASSERT_NE(pixels, nullptr);
        auto data = static_cast<uint8_t *>(pixels);
        for (int offset = 0; offset < size; offset += 4) {
          std::copy(readback_color.begin(), readback_color.end(), data + offset);
        }
      };
      wl::test::init_display = [&](platf::display_t &display) {
        // Output announcements use physical current-mode pixels, even when
        // the logical desktop has fractional scaling or more modes follow.
        wl::monitor_t monitor {nullptr};
        monitor.wl_mode(nullptr, WL_OUTPUT_MODE_CURRENT, source_width, source_height, 60000);
        monitor.xdg_size(nullptr, source_width * 4 / 5, source_height * 4 / 5);
        monitor.wl_mode(nullptr, WL_OUTPUT_MODE_PREFERRED, 3840, 2160, 60000);
        display.width = monitor.viewport.width;
        display.height = monitor.viewport.height;
        return 0;
      };
    }

    void TearDown() override {
      wl::test::init_display = {};
      wl::test::capture_frame = {};
      inject_codec_sink = false;
      converted_pixels.clear();
      inject_import = false;
      gl::ctx = saved_gl;
      config::video.min_threads = saved_threads;
    }

    std::shared_ptr<platf::display_t> make_display() {
      video::config_t stream {2560, 1440, 60};
      return platf::wl_display(platf::mem_type_e::cuda, "0", stream);
    }

    void capture_frame(platf::display_t &display, const std::shared_ptr<platf::img_t> &image) {
      readback_width = source_width;
      readback_height = source_height;
      wl::test::capture_frame = [&](egl::surface_descriptor_t &source) {
        source.width = source_width;
        source.height = source_height;
        return platf::capture_e::ok;
      };
      bool cursor = false;
      unsigned pushes = 0;
      EXPECT_EQ(display.capture([&](std::shared_ptr<platf::img_t> &&captured, bool frame_captured) {
        ++pushes;
        EXPECT_TRUE(frame_captured);
        EXPECT_EQ(captured, image);
        EXPECT_TRUE(captured->frame_timestamp.has_value());
        return false;
      },
                                [&](std::shared_ptr<platf::img_t> &output) {
                                  output = image;
                                  return true;
                                },
                                &cursor),
                platf::capture_e::ok);
      EXPECT_EQ(pushes, 1);
    }

    int source_width = 2560;
    int source_height = 1440;
    int saved_threads;
    GladGLContext saved_gl;
  };

  TEST_F(WlrRamContract, CurrentNativeModeSurvivesLogicalSizeAndOtherModes) {
    auto display = make_display();
    ASSERT_NE(display, nullptr);
    EXPECT_EQ(display->width, 2560);
    EXPECT_EQ(display->height, 1440);
  }

  TEST_F(WlrRamContract, UnsupportedMemoryTypeIsRejectedBeforeInitialization) {
    EXPECT_EQ(platf::wl_display(platf::mem_type_e::unknown, "0", video::config_t {2560, 1440, 60}), nullptr);
  }

  #ifdef SUNSHINE_BUILD_VAAPI
  TEST_F(WlrRamContract, VaapiStillUsesDmaBufImages) {
    auto display = platf::wl_display(platf::mem_type_e::vaapi, "0", video::config_t {2560, 1440, 60});
    ASSERT_NE(display, nullptr);
    auto image = display->alloc_img();
    ASSERT_NE(image, nullptr);
    EXPECT_NE(dynamic_cast<egl::img_descriptor_t *>(image.get()), nullptr);
    EXPECT_EQ(image->data, nullptr);
  }
  #endif

  #ifndef SUNSHINE_BUILD_CUDA
  TEST_F(WlrRamContract, NvencWithoutCudaProvidesCpuPixelsAtSourceGeometry) {
    auto display = make_display();
    ASSERT_NE(display, nullptr);
    auto image = display->alloc_img();
    ASSERT_NE(image, nullptr);
    ASSERT_NE(image->data, nullptr);  // A DMA-BUF descriptor violates this contract.
    EXPECT_EQ(image->width, 2560);
    EXPECT_EQ(image->height, 1440);
    EXPECT_EQ(image->pixel_pitch, 4);
    EXPECT_EQ(image->row_pitch, 2560 * 4);
    auto device = display->make_avcodec_encode_device(platf::pix_fmt_e::nv12);
    ASSERT_NE(device, nullptr);
    EXPECT_EQ(device->data, nullptr);  // Select shared CPU conversion/hardware upload.
  }

  TEST_F(WlrRamContract, DummyClearsReusedPixelsWithoutCapture) {
    auto display = make_display();
    ASSERT_NE(display, nullptr);
    auto image = display->alloc_img();
    ASSERT_NE(image->data, nullptr);
    auto size = static_cast<std::size_t>(image->row_pitch) * image->height;
    std::fill_n(image->data, size, 0xa5);
    ASSERT_EQ(display->dummy_img(image.get()), 0);
    EXPECT_TRUE(std::all_of(image->data, image->data + size, [](auto pixel) {
      return pixel == 0;
    }));
    EXPECT_FALSE(image->frame_timestamp.has_value());
    EXPECT_EQ(readback_calls, 0);
  }

  TEST_F(WlrRamContract, NativeSizeChangeRequestsReinitBeforeReadback) {
    auto display = make_display();
    ASSERT_NE(display, nullptr);
    wl::test::capture_frame = [&](egl::surface_descriptor_t &source) {
      source.width = source_width + 1;
      source.height = source_height;
      return platf::capture_e::ok;
    };
    bool cursor = false;
    bool pulled = false, pushed = false;
    EXPECT_EQ(display->capture([&](std::shared_ptr<platf::img_t> &&, bool) {
      pushed = true;
      return false;
    },
                               [&](std::shared_ptr<platf::img_t> &) {
                                 pulled = true;
                                 return false;
                               },
                               &cursor),
              platf::capture_e::reinit);
    EXPECT_FALSE(pulled);
    EXPECT_FALSE(pushed);
    EXPECT_EQ(readback_calls, 0);
  }

  struct ConversionCase {
    int source_width, source_height, target_width, target_height;
    AVPixelFormat format;
    unsigned bit_depth;
  };

  class WlrRamConversion: public WlrRamContract, public testing::WithParamInterface<ConversionCase> {};

  TEST_P(WlrRamConversion, ProbeThenFrameConvertsToExpectedYuv) {
    auto params = GetParam();
    source_width = params.source_width;
    source_height = params.source_height;
    auto display = make_display();
    ASSERT_NE(display, nullptr);
    ASSERT_EQ(display->width, source_width);
    ASSERT_EQ(display->height, source_height);
    auto image = display->alloc_img();
    ASSERT_NE(image->data, nullptr);

    video::encoder_t::codec_t codec;
    // The bundled FFmpeg deliberately omits rawvideo. Use an available codec
    // descriptor and intercept only codec I/O; CPU conversion remains real.
    codec.name = "libx264";
    codec.capabilities.set();
    video::encoder_t cpu_sink {
      "wlr-cpu-contract",
      std::make_unique<video::encoder_platform_formats_avcodec>(
        AV_HWDEVICE_TYPE_NONE,
        AV_HWDEVICE_TYPE_NONE,
        AV_PIX_FMT_NONE,
        params.format,
        params.format,
        AV_PIX_FMT_NONE,
        AV_PIX_FMT_NONE,
        video::encoder_platform_formats_avcodec::init_buffer_function_t {}
      ),
      codec,
      codec,
      codec,
      0
    };
    video::config_t stream {params.target_width, params.target_height, 60, 1000, 1, 0, 2, 1, params.bit_depth == 10 ? 1 : 0, 0};
    auto device = video::make_encode_device(*display, cpu_sink, stream);
    ASSERT_NE(device, nullptr);
    inject_codec_sink = true;
    auto session = video::make_encode_session(display.get(), cpu_sink, stream, image->width, image->height, std::move(device));
    ASSERT_NE(session, nullptr);

    auto packets = mail::man->queue<video::packet_t>("wlr-ram-cpu-contract");
    auto convert_and_check = [&](int index, int expected_y, int expected_u, int expected_v) {
      ASSERT_EQ(session->convert(*image), 0);
      ASSERT_EQ(video::encode(index, *session, packets, nullptr, {}), 0);
      const auto *data = converted_pixels.data();
      int x = params.target_width / 2;
      int y = params.target_height / 2;
      auto sample = [&](int px, int py) {
        auto offset = py * params.target_width + px;
        return params.bit_depth == 10 ? reinterpret_cast<const uint16_t *>(data)[offset] >> 6 : data[offset];
      };
      auto plane_size = params.target_width * params.target_height;
      ASSERT_GE(converted_pixels.size(), static_cast<std::size_t>(plane_size) * (params.bit_depth == 10 ? 2 : 1) * 3 / 2);
      auto chroma_offset = (y / 2) * (params.target_width / 2) + x / 2;
      int u, v;
      if (params.format == AV_PIX_FMT_YUV420P) {
        u = data[plane_size + chroma_offset];
        v = data[plane_size + plane_size / 4 + chroma_offset];
      } else if (params.bit_depth == 10) {
        auto chroma = reinterpret_cast<const uint16_t *>(data) + plane_size + chroma_offset * 2;
        u = chroma[0] >> 6;
        v = chroma[1] >> 6;
      } else {
        auto chroma = data + plane_size + chroma_offset * 2;
        u = chroma[0];
        v = chroma[1];
      }
      auto depth_scale = params.bit_depth == 10 ? 4 : 1;
      EXPECT_NEAR(sample(x, y), expected_y * depth_scale, 3);
      EXPECT_NEAR(u, expected_u * depth_scale, 3);
      EXPECT_NEAR(v, expected_v * depth_scale, 3);
      // The ultrawide input is letterboxed, with initialized black padding.
      if (params.source_width * params.target_height > params.target_width * params.source_height) {
        EXPECT_NEAR(sample(x, 0), params.bit_depth == 10 ? 64 : 16, 2);
        EXPECT_NEAR(sample(x, params.target_height - 1), params.bit_depth == 10 ? 64 : 16, 2);
      }
    };

    ASSERT_EQ(display->dummy_img(image.get()), 0);
    convert_and_check(1, 16, 128, 128);
    // Drive the real RAM snapshot/readback with injected DMA-BUF/GL I/O.
    // White RGB with zero alpha checks that BGR0 ignores the fourth byte.
    readback_color = {255, 255, 255, 0};
    capture_frame(*display, image);
    convert_and_check(2, 235, 128, 128);
    // Saturated red distinguishes BGRA from RGBA and checks UV ordering.
    readback_color = {0, 0, 255, 0};
    capture_frame(*display, image);
    convert_and_check(3, 63, 102, 240);
    EXPECT_EQ(readback_calls, 2);
  }

  INSTANTIATE_TEST_SUITE_P(NvencInputFormats, WlrRamConversion, testing::Values(ConversionCase {2560, 1440, 1920, 1080, AV_PIX_FMT_NV12, 8},  // Startup probe.
                                                                                ConversionCase {2560, 1440, 2560, 1440, AV_PIX_FMT_NV12, 8},  // HDMI target, scale 1.
                                                                                ConversionCase {3440, 1440, 2560, 1440, AV_PIX_FMT_P010LE, 10},  // Native ultrawide, padding.
                                                                                ConversionCase {2560, 1440, 1920, 1080, AV_PIX_FMT_YUV420P, 8}));  // Software path.
  #endif
}  // namespace
#endif

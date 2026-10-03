/** @brief Mapped metadata bounds for the pinned encoder's five-level 4:2:0 layout. */
#pragma once

#include "src/pyrowave_protocol.h"

namespace pyrowave {
  // Mirrors WaveletBuffers::init_block_meta at pin 5e4a98f. One eight-byte
  // metadata record per 32x32 subband block, including zero-length blocks.
  inline size_t raw_block_count(Dimensions output) {
    if (!output.supported()) {
      throw std::runtime_error("Unsupported Pyrowave encoder dimensions");
    }
    const size_t aligned_width = (output.width + 31) / 32 * 32;
    const size_t aligned_height = (output.height + 31) / 32 * 32;
    size_t count = 0;
    for (size_t level = 0; level < 5; ++level) {
      const size_t width = aligned_width >> (level + 1);
      const size_t height = aligned_height >> (level + 1);
      const size_t components = level == 0 ? 1 : 3;  // 420 omits top-level chroma.
      const size_t bands = level == 4 ? 4 : 3;  // Only coarsest level includes LL.
      count += (width + 31) / 32 * ((height + 31) / 32) * components * bands;
    }
    return count;
  }
}  // namespace pyrowave

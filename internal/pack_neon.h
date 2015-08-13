// Copyright 2015 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// pack_neon.h: optimized NEON specializations of the templates in pack.h.

#ifndef GEMMLOWP_INTERNAL_PACK_NEON_H_
#define GEMMLOWP_INTERNAL_PACK_NEON_H_

#include "pack.h"

#include <arm_neon.h>

namespace gemmlowp {

typedef SideMap<const std::uint8_t, SideMapOrder::WidthMajor> WidthMajorUint8SideMap;

template <int Cells>
using DepthMajorSideFormatNCells4x2 = KernelSideFormat<CellFormat<4, 2>, Cells>;

template <int Cells>
class PackingRegisterBlock<WidthMajorUint8SideMap, DepthMajorSideFormatNCells4x2<Cells> >
  : public PackingRegisterBlockBase<WidthMajorUint8SideMap, DepthMajorSideFormatNCells4x2<Cells> >
{
 public:
  typedef DepthMajorSideFormatNCells4x2<Cells> KernelSideFormat;
  typedef typename KernelSideFormat::Cell CellFormat;
  static const int kCells = KernelSideFormat::kCells;
  static const int kCellWidth = CellFormat::kWidth;
  static const int kKernelWidth = CellFormat::kWidth * kCells;
  static const int kCellDepth = CellFormat::kDepth;
  static const int kCellSize = CellFormat::kSize;

  void Store(PackedSideBlock<KernelSideFormat>* dst, int start_width) {
    std::uint8_t* dst_ptr = dst->current_data();
    const std::uint8_t* const src_ptr = this->loaded_src_.data();
    const int stride = this->loaded_src_.stride();
    // Load raw source WidthMajor data
    uint8x16_t src_lines[4 * kCells];
    for (int i = 0; i < 4 * kCells; i++) {
      src_lines[i] = vld1q_u8(src_ptr + i * stride);
    }
    // Reorder the data within registers to make DepthMajor 4x2 cells
    uint8x16x2_t src_lines_intertwined_2x[2 * kCells];
    for (int i = 0; i < kCells; i++) {
      src_lines_intertwined_2x[2 * i] = vzipq_u8(src_lines[4 * i], src_lines[4 * i + 2]);
      src_lines_intertwined_2x[2 * i + 1] = vzipq_u8(src_lines[4 * i + 1], src_lines[4 * i + 3]);
    }
    uint8x16x2_t src_lines_intertwined_4x[2 * kCells];
    for (int i = 0; i < kCells; i++) {
      src_lines_intertwined_4x[2 * i] = vzipq_u8(src_lines_intertwined_2x[2 * i].val[0], src_lines_intertwined_2x[2 * i + 1].val[0]);
      src_lines_intertwined_4x[2 * i + 1] = vzipq_u8(src_lines_intertwined_2x[2 * i].val[1], src_lines_intertwined_2x[2 * i + 1].val[1]);
    }
    // Store the resulting DepthMajor 4x2 cells in the destination packed block
    for (int outer = 0; outer < 2; outer++) {
      for (int inner = 0; inner < 2; inner++) {
        for (int cell = 0; cell < kCells; cell++) {
          vst1_u8(dst_ptr, vget_low_u8(src_lines_intertwined_4x[2 * cell + outer].val[inner]));
          dst_ptr += 8;
        }
        for (int cell = 0; cell < kCells; cell++) {
          vst1_u8(dst_ptr, vget_high_u8(src_lines_intertwined_4x[2 * cell + outer].val[inner]));
          dst_ptr += 8;
        }
      }
    }
    // Compute sums across the depth dimension
    uint16x8_t sums_of_2_cells[kCells][4];
    for (int outer = 0; outer < 2; outer++) {
      for (int inner = 0; inner < 2; inner++) {
        int i = 2 * outer + inner;
        for (int cell = 0; cell < kCells; cell++) {
          sums_of_2_cells[cell][i] = vaddl_u8(vget_low_u8(src_lines_intertwined_4x[2 * cell + outer].val[inner]),
                                              vget_high_u8(src_lines_intertwined_4x[2 * cell + outer].val[inner]));
        }
      }
    }
    int32x4_t sums_of_4_cells[kCells][4];
    for (int i = 0; i < 4; i++) {
      for (int cell = 0; cell < kCells; cell++) {
        sums_of_4_cells[cell][i] = vreinterpretq_s32_u32(vaddl_u16(vget_low_u16(sums_of_2_cells[cell][i]), vget_high_u16(sums_of_2_cells[cell][i])));
      }
    }
    // Update the rank_one_update vector
    for (int cell = 0; cell < kCells; cell++) {
      int32x4_t s01 = vaddq_s32(sums_of_4_cells[cell][0], sums_of_4_cells[cell][1]);
      int32x4_t s23 = vaddq_s32(sums_of_4_cells[cell][2], sums_of_4_cells[cell][3]);
      int32x4_t s = vaddq_s32(s01, s23);
      int32x4_t u = vmulq_n_s32(s, dst->rank_one_update_multiplier());
      std::int32_t* rank_one_update_ptr = dst->rank_one_update() + start_width + 4 * cell;
      vst1q_s32(rank_one_update_ptr, vaddq_s32(u, vld1q_s32(rank_one_update_ptr)));
    }
    dst->seek_forward_n_cells(kCells * kRegisterSize / kCellDepth);
  }
};

}  // namespace gemmlowp

#endif  // GEMMLOWP_INTERNAL_PACK_NEON_H_

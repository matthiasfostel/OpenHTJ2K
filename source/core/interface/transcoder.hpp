// Copyright (c) 2019 - 2026, Osamu Watanabe
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
//    modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
//    IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
//    FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
//    DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
//    SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
//    CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// EXPERIMENTAL (prototype): transcoding of a JPEG 2000 Part 1 (EBCOT/MQ) codestream into an
// HTJ2K (Part 15) codestream, in the coefficient domain.
//
// The Part 1 code-blocks are decoded to their quantisation indices and the indices are coded
// again with the HT block coder.  There is no wavelet transform, no re-quantisation and no
// colour transform, so the image a decoder reconstructs from the result is the one it
// reconstructs from the input, except for code-blocks that a lossy Part 1 encoder cut
// part-way through a bit-plane (see transcode_partial_plane).  The marker segments that describe the image (SIZ, QCD, QCC,
// COM, tiling, precincts, progression order, SOP/EPH) are kept; the code-block style becomes HT,
// the CAP marker segment is added, and the packets are written again, with one quality layer.
//
// Prototype limits (the function throws, with a message, when the input has one):
//   raw codestream input (no JP2 file format), no COC, RGN, POC, PPM, DFS or ATK marker
//   segments, no tile-part header COD/QCD/COC/QCC overrides, Part 1 block coder input.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include "decoder.hpp"  // OPENHTJ2K_EXPORT

namespace open_htj2k {
// A Part 1 code-block is coded down to some pass.  When that is a cleanup pass, every sample
// is known down to that bit-plane, and the HT block coder codes the block at that plane: the
// HT decoder's bin centre then equals the Part 1 mid-point, so the reconstruction is the same
// and the bit-planes below the cut cost nothing.  When the last pass is a SigProp or MagRef
// pass, the block ends part-way through a bit-plane, which one HT cleanup pass cannot hold.
enum class transcode_partial_plane {
  keep,  // code the indices to the last bit, partial plane included (reconstruction off by half a step)
  drop   // code the block at its last complete cleanup plane and drop the partial plane
};

// What the code-blocks that had coding passes were found to end with.
struct transcode_stats {
  uint64_t blocks  = 0;  // code-blocks with coding passes
  uint64_t full    = 0;  // decoded to the last bit-plane, ending on a cleanup pass
  uint64_t coarser = 0;  // ending on a cleanup pass above the last bit-plane
  uint64_t partial = 0;  // ending part-way through a bit-plane
};

struct transcode_options {
  transcode_partial_plane partial_plane = transcode_partial_plane::keep;
  transcode_stats *stats                = nullptr;  // optional: receives the block statistics
};

// Transcodes the Part 1 codestream [in, in + in_len) into the HTJ2K codestream `out`.
// Returns the size of `out`.  Throws std::exception on input it cannot transcode.
OPENHTJ2K_EXPORT size_t transcode_part1_to_htj2k(const uint8_t *in, size_t in_len, std::vector<uint8_t> &out);
OPENHTJ2K_EXPORT size_t transcode_part1_to_htj2k(const uint8_t *in, size_t in_len, std::vector<uint8_t> &out,
                                                 const transcode_options &options);
}  // namespace open_htj2k

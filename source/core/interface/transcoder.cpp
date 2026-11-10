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

#include <algorithm>
#include <atomic>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <cstring>
#include <memory>
#include <vector>
#include "transcoder.hpp"
#include "jph.hpp"
#include "coding_units.hpp"
#include "block_decoding.hpp"
#include "message_internal.hpp"
#ifdef OPENHTJ2K_THREAD
  #include "ThreadPool.hpp"
#endif

namespace open_htj2k {
namespace {

// Bits 0-4 of Ccap15 for a magnitude bound B (ISO/IEC 15444-15, A.3.7); see encoder.cpp.
uint16_t magb_field(int32_t bound) {
  if (bound <= 8) return 0;
  if (bound <= 27) return static_cast<uint16_t>(bound - 8);
  if (bound <= 71) return static_cast<uint16_t>(19 + (bound - 27 + 3) / 4);
  return 31;
}

// Per-thread scratch of the block transcoder.
struct scratch_t {
  std::vector<int32_t> dec;   // decoded Part 1 samples: sign in bit 31, magnitude below
  std::vector<uint32_t> ctx;  // Part 1 context words
  std::vector<int32_t> enc;   // the same samples in the form the HT encoder takes
};

// Decodes the Part 1 code-block `bin` to its quantisation indices and codes them with the HT
// block coder into `bout`, the same code-block of the output tile.
void transcode_block(j2k_codeblock *bin, j2k_codeblock *bout, scratch_t &sc,
                     const transcode_options &opt, transcode_stats &stats) {
  const uint32_t W      = bin->size.x;
  const uint32_t H      = bin->size.y;
  const uint32_t stride = round_up(W, 8U);
  const uint32_t QH     = round_up(H, 8U);

  uint32_t or_val = 0;
  uint32_t p_c    = 0;  // bit-plane (above the index LSB) the HT cleanup pass is coded at
  bool refine     = false;  // the partial plane goes into HT SigProp (and MagRef) passes
  bool sp_only    = false;  // ... and the Part 1 block ended after its SigProp pass
  if (bin->num_passes > 0 && bin->get_compressed_data() != nullptr) {
    sc.dec.assign(static_cast<size_t>(stride) * QH, 0);
    sc.ctx.assign(static_cast<size_t>(QH / 4 + 2) * (stride + 2), 0);
    bin->sample_buf            = sc.dec.data();
    bin->blksampl_stride       = stride;
    bin->block_contexts        = sc.ctx.data();
    bin->block_contexts_stride = stride + 2;
    j2k_decode_indices(bin, 0);

    // The decoder holds a magnitude in Q(31 - M_b): its index is the sample shifted down by
    // pLSB = 31 - M_b (j2k_dequant does the same for the reversible transform).  A marker below
    // that bit is the half-LSB mid-point and is dropped; one at or above it belongs to a sample
    // a truncated stream decoded to a coarser bit-plane, and stays in the index.
    // The HT encoder takes 0x80000000 | ((m - 1) << 1) | sign for an index m > 0, in a buffer
    // padded to multiples of 8 in both directions.
    const int32_t Mb   = static_cast<int32_t>(bin->get_Mb());
    const int32_t pLSB = 31 - Mb;
    if (pLSB < 0) open_htj2k::detail::fail("transcoder: a sub-band has more than 31 magnitude bit-planes.");

    // The passes are the cleanup pass of the first bit-plane, then SigProp, MagRef and cleanup
    // for each further one.  After n passes the cleanup passes of (n + 2) / 3 bit-planes are
    // done, so every sample is known down to the bit-plane `last_plane` (an index bit-plane,
    // 0 being the LSB); a block that ends on its SigProp or MagRef pass has some samples known
    // one plane further.
    const uint32_t n      = bin->num_passes;
    const int32_t planes  = static_cast<int32_t>((n + 2) / 3);
    const int32_t last_pl = Mb - static_cast<int32_t>(bin->num_ZBP) - planes;
    if (last_pl < 0) open_htj2k::detail::fail("transcoder: a code-block has more passes than bit-planes.");
    const bool partial = ((n - 1) % 3) != 0;
    ++stats.blocks;
    if (partial) {
      ++stats.partial;
    } else if (last_pl == 0) {
      ++stats.full;
    } else {
      ++stats.coarser;
    }
    // Ending on a cleanup pass, or dropping the partial plane, or carrying it in HT refinement
    // passes: the HT cleanup pass is coded at that plane.  Otherwise (keep) every decoded bit is
    // coded, down to plane 0.
    if (!partial || opt.partial_plane != transcode_partial_plane::keep) p_c = static_cast<uint32_t>(last_pl);
    // The HT set of cleanup at plane p, SigProp and MagRef of plane p - 1 is the Part 1 sequence
    // cleanup, SigProp, MagRef of the plane below.  A block that ended after SigProp has the
    // set cut there (the refinement segment up to the SigProp bits).
    refine  = partial && opt.partial_plane == transcode_partial_plane::refine && p_c >= 1;
    sp_only = refine && (n % 3 == 2);

    // Fills the encoder's samples from the decoded buffer: the magnitude is shifted down by
    // `sh` bits (the bits below, the markers among them, are gone) and up again by `up`.
    // Returns the largest magnitude.
    auto fill = [&](int32_t sh, uint32_t up) -> uint32_t {
      uint32_t mx = 0;
      or_val      = 0;
      sc.enc.assign(static_cast<size_t>(stride) * QH, 0);
      for (uint32_t y = 0; y < H; ++y) {
        const uint32_t *sp = reinterpret_cast<const uint32_t *>(sc.dec.data()) + static_cast<size_t>(y) * stride;
        int32_t *dp        = sc.enc.data() + static_cast<size_t>(y) * stride;
        for (uint32_t x = 0; x < W; ++x) {
          const uint32_t mu = ((sp[x] & 0x7FFFFFFFu) >> sh) << up;
          or_val |= mu;
          mx = std::max(mx, mu);
          dp[x] = mu ? static_cast<int32_t>(0x80000000u | ((mu - 1) << 1) | (sp[x] >> 31)) : 0;
        }
      }
      return mx;
    };

    if (refine) {
      // Index bit-plane q = p_c - 1 of the decoder's buffer is bit pLSB + q.  The samples keep
      // their bits down to q, which the SigProp and MagRef passes code; the Part 1 mid-point
      // markers below q are cleared.
      const uint32_t q  = p_c - 1;
      const uint32_t mx = fill(pLSB + static_cast<int32_t>(q), q);
      // The HT encoder keeps the cleanup plane below the block's top bit-plane (and within M_b);
      // a block that does not allow p_c cannot be coded as a set, and drops the plane instead.
      const uint32_t msb = mx ? 32u - count_leading_zeros(mx) : 0u;
      if (p_c + 1 > std::min(msb, static_cast<uint32_t>(Mb))) {
        refine  = false;
        sp_only = false;
      }
    }
    if (refine) {
      ++stats.refined;
    } else {
      // Index bit-plane p_c of the decoder's buffer is bit pLSB + p_c; the markers below it (the
      // mid-point of the last plane) are shifted out.
      fill(pLSB + static_cast<int32_t>(p_c), 0);
    }
    bin->sample_buf     = nullptr;
    bin->block_contexts = nullptr;
  }

  bout->sample_buf        = or_val ? sc.enc.data() : nullptr;
  bout->blksampl_stride   = stride;
  bout->pre_quantized     = true;
  bout->pre_or_val        = or_val ? 1 : 0;
  bout->ht_refine_planes  = refine ? static_cast<uint8_t>(p_c) : 0;
  bout->refsegment        = false;
  if (refine) {
    // Cleanup at plane p_c, then SigProp and MagRef of plane p_c - 1 in one refinement segment;
    // the encoder signals num_ZBP = M_b - 1 - p_c itself.
    ht_refinement_info rinfo;
    htj2k_encode_refined(bout, 0, &rinfo);
    bout->sample_buf = nullptr;
    bout->ht_refine_planes = 0;
    if (sp_only && bout->num_passes == 3) {
      // The Part 1 block ended after its SigProp pass: keep the cleanup segment and the
      // refinement segment as far as the SigProp bits, whose last byte stands alone (the cut
      // rate control makes).  Without SigProp bits it is the cleanup pass alone.
      const uint32_t Lref = bout->pass_length[2];
      const uint32_t Lcup = bout->length - Lref;
      const uint8_t passes = (rinfo.Lsp != 0) ? 2 : 1;
      bout->layer_start[0]    = 0;
      bout->layer_passes[0]   = passes;
      bout->num_passes        = passes;
      bout->pass_length[0]    = Lcup;
      bout->pass_length[1]    = (passes == 2) ? rinfo.Lsp : 0;
      bout->pass_length[2]    = 0;
      bout->pass_length_count = passes;
      bout->length            = Lcup + ((passes == 2) ? rinfo.Lsp : 0);
      if (passes == 2) bout->get_compressed_data()[Lcup + rinfo.Lsp - 1] = rinfo.sp_tail;
    }
    return;
  }
  encode_codeblock(bout, 0);
  bout->sample_buf = nullptr;
  // The cleanup pass codes the magnitudes of the shifted indices, whatever plane they stand for;
  // the plane only enters through the number of missing MSBs the packet header signals,
  // M_b - 1 - p (ISO/IEC 15444-15, 7.6; the same as the encoder's refinement path sets).
  if (p_c > 0 && bout->num_passes > 0) bout->num_ZBP = static_cast<uint8_t>(bout->get_Mb() - 1 - p_c);
}

// Calls fn(block_in, block_out) for every code-block, the two tiles being of the same structure.
template <typename Fn>
void for_each_block_pair(j2k_tile &tin, j2k_tile &tout, Fn fn) {
  for (uint16_t c = 0; c < tin.get_num_components(); ++c) {
    j2k_tile_component *tci = tin.get_tile_component(c);
    j2k_tile_component *tco = tout.get_tile_component(c);
    const uint8_t NL        = tci->get_dwt_levels();
    if (tco->get_dwt_levels() != NL) open_htj2k::detail::fail("transcoder: tile structures differ (levels).");
    for (uint8_t r = 0; r <= NL; ++r) {
      j2k_resolution *cri = tci->access_resolution(r);
      j2k_resolution *cro = tco->access_resolution(r);
      if (cri->is_empty != cro->is_empty || cri->npw != cro->npw || cri->nph != cro->nph
          || cri->num_bands != cro->num_bands) {
        open_htj2k::detail::fail("transcoder: tile structures differ (resolution %u).", unsigned(r));
      }
      if (cri->is_empty) continue;
      for (uint32_t pi = 0; pi < cri->npw * cri->nph; ++pi) {
        j2k_precinct *cpi = cri->access_precinct(pi);
        j2k_precinct *cpo = cro->access_precinct(pi);
        for (uint8_t b = 0; b < cri->num_bands; ++b) {
          j2k_precinct_subband *pbi = cpi->access_pband(b);
          j2k_precinct_subband *pbo = cpo->access_pband(b);
          const uint32_t n          = pbi->num_codeblock_x * pbi->num_codeblock_y;
          if (n != pbo->num_codeblock_x * pbo->num_codeblock_y) {
            open_htj2k::detail::fail("transcoder: tile structures differ (code-blocks).");
          }
          for (uint32_t i = 0; i < n; ++i) fn(pbi->access_codeblock(i), pbo->access_codeblock(i));
        }
      }
    }
  }
}

// The magnitude bound B of the transcoded HT code-blocks (ISO/IEC 15444-15, 8.7.3); the same
// computation as the encoder's coded_MAGB().
int32_t coded_magb(j2k_tile *tiles, uint32_t num_tiles) {
  int32_t B = 0;
  for (uint32_t t = 0; t < num_tiles; ++t) {
    for (uint16_t c = 0; c < tiles[t].get_num_components(); ++c) {
      j2k_tile_component *tc = tiles[t].get_tile_component(c);
      const int32_t NL       = tc->get_dwt_levels();
      for (uint8_t r = 0; r <= NL; ++r) {
        j2k_resolution *cr = tc->access_resolution(r);
        if (cr->is_empty) continue;
        const int32_t nb = (r == 0) ? NL : NL - r + 1;
        for (uint32_t pi = 0; pi < cr->npw * cr->nph; ++pi) {
          j2k_precinct *cp = cr->access_precinct(pi);
          for (uint8_t b = 0; b < cr->num_bands; ++b) {
            j2k_precinct_subband *cpb = cp->access_pband(b);
            const uint32_t n          = cpb->num_codeblock_x * cpb->num_codeblock_y;
            for (uint32_t i = 0; i < n; ++i) {
              const j2k_codeblock *block = cpb->access_codeblock(i);
              if ((block->Cmodes & HT) == 0 || block->num_passes == 0) continue;
              B = std::max<int32_t>(B, block->num_ZBP + 1 - ((block->transformation == 1) ? 0 : nb - 1));
            }
          }
        }
      }
    }
  }
  return B;
}

void check_supported(const j2k_main_header &h) {
  if (!h.COC.empty()) open_htj2k::detail::fail("transcoder: COC marker segments are not supported yet.");
  if (!h.RGN.empty()) open_htj2k::detail::fail("transcoder: RGN (ROI) marker segments are not supported.");
  if (h.POC != nullptr) open_htj2k::detail::fail("transcoder: POC marker segments are not supported yet.");
  if (!h.PPM.empty()) open_htj2k::detail::fail("transcoder: PPM marker segments are not supported yet.");
  if (!h.DFS.empty() || !h.ATK.empty())
    open_htj2k::detail::fail("transcoder: Part 2 (DFS/ATK) marker segments are not supported yet.");
  if (h.COD == nullptr || h.QCD == nullptr || h.SIZ == nullptr)
    open_htj2k::detail::fail("transcoder: the main header lacks SIZ, COD or QCD.");
  if (h.COD->get_Cmodes() & HT)
    open_htj2k::detail::fail("transcoder: the input already uses the HT block coder.");
}

// Rejects a codestream whose tile-part headers hold COD, COC, QCD, QCC or RGN marker segments.
// The output tiles take their coding and quantisation parameters from the main header, so a tile
// that overrides them would be written with the wrong step sizes and decode to a wrong image
// (the main-header-only check above cannot see these).  Walks the segments by their lengths.
void check_tile_part_headers(const uint8_t *in, size_t len) {
  auto u16 = [&](size_t p) -> uint32_t { return (static_cast<uint32_t>(in[p]) << 8) | in[p + 1]; };
  size_t pos = 2;  // after SOC
  while (pos + 4 <= len && u16(pos) != _SOT) pos += 2 + u16(pos + 2);  // main header segments
  while (pos + 12 <= len && u16(pos) == _SOT) {
    const size_t psot = (static_cast<size_t>(in[pos + 6]) << 24) | (static_cast<size_t>(in[pos + 7]) << 16)
                        | (static_cast<size_t>(in[pos + 8]) << 8) | in[pos + 9];
    const size_t end = (psot == 0) ? len : std::min(len, pos + psot);  // Psot 0: up to EOC
    size_t p         = pos + 12;                                       // after the SOT segment
    while (p + 4 <= end && u16(p) != _SOD) {
      const uint32_t m = u16(p);
      if (m == _COD || m == _COC || m == _QCD || m == _QCC || m == _RGN) {
        open_htj2k::detail::fail(
            "transcoder: a tile-part header with a COD, COC, QCD, QCC or RGN marker segment (tile %u) is "
            "not supported yet.",
            static_cast<unsigned>(u16(pos + 4)));
      }
      p += 2 + u16(p + 2);
    }
    pos = end;
  }
}

// Reads the main header of a codestream held in `in` (positioned on SOC).
void read_main_header(j2c_src_memory &src, const uint8_t *data, size_t len, j2k_main_header &hdr) {
  src.alloc_memory(len);
  std::memcpy(src.get_buf_pos(), data, len);
  hdr.read(src);
  src.rewind_2bytes();
}


uint32_t be32(const uint8_t *p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// The boxes of a JP2 file that the JPH file keeps: those before the codestream box (the
// signature box, the file type box and the codestream box excepted) go to `pre`, those after it to
// `post`.  Only the top level is walked; a box is copied as it is, header included.
void collect_file_boxes(const uint8_t *buf, size_t len, std::vector<uint8_t> &pre, std::vector<uint8_t> &post) {
  const uint8_t *p = buf, *end = buf + len;
  bool seen_jp2c   = false;
  while (static_cast<size_t>(end - p) >= 8) {
    const size_t remaining = static_cast<size_t>(end - p);
    const uint32_t lbox    = be32(p);
    const uint32_t tbox    = be32(p + 4);
    size_t blen;
    if (lbox == 1) {
      if (remaining < 16) break;
      uint64_t x = 0;
      for (int i = 0; i < 8; ++i) x = (x << 8) | p[8 + i];
      if (x < 16 || x > remaining) break;
      blen = static_cast<size_t>(x);
    } else if (lbox == 0) {
      blen = remaining;
    } else {
      if (lbox < 8) break;
      blen = (lbox <= remaining) ? lbox : remaining;
    }
    if (tbox == 0x6A703263u) {  // jp2c
      seen_jp2c = true;
    } else if (tbox != 0x6A502020u && tbox != 0x66747970u) {  // not jP, ftyp
      std::vector<uint8_t> &dst = seen_jp2c ? post : pre;
      dst.insert(dst.end(), p, p + blen);
    }
    p += blen;
  }
}

}  // namespace

size_t transcode_part1_to_htj2k(const uint8_t *in, size_t in_len, std::vector<uint8_t> &out) {
  return transcode_part1_to_htj2k(in, in_len, out, transcode_options());
}

size_t transcode_part1_to_htj2k(const uint8_t *in, size_t in_len, std::vector<uint8_t> &out,
                                const transcode_options &options) {
  transcode_stats stats;
  // A JP2 file: transcode its codestream, keep its other boxes for a JPH output.
  const uint8_t *file_in = in;
  const size_t file_len  = in_len;
  jph_info finfo;
  const bool is_file = (in != nullptr) && jph_parse_buffer(in, in_len, finfo);
  if (is_file) {
    in     = finfo.cs_data;
    in_len = finfo.cs_size;
  }
  if (in == nullptr || in_len < 4 || in[0] != 0xFF || in[1] != 0x4F) {
    open_htj2k::detail::fail("transcoder: the input is neither a JP2/JPH file nor a raw JPEG 2000 codestream.");
  }

  // Input: main header and tiles with their packets parsed.  Nothing is decoded here.
  j2c_src_memory src;
  j2k_main_header hin;
  read_main_header(src, in, in_len, hin);
  check_supported(hin);
  check_tile_part_headers(in, in_len);

  element_siz numTiles;
  hin.get_number_of_tiles(numTiles.x, numTiles.y);
  const uint32_t num_tiles = numTiles.x * numTiles.y;
  if (num_tiles == 0 || num_tiles > 65535) open_htj2k::detail::fail("transcoder: bad number of tiles.");

  std::vector<j2k_tile> tin(num_tiles);
  for (uint32_t i = 0; i < num_tiles; ++i) tin[i].dec_init(static_cast<uint16_t>(i), hin, 0);
  {
    uint16_t word;
    while (src.get_remaining() >= 2 && (word = src.get_word()) != _EOC) {
      if (word != _SOT) open_htj2k::detail::fail("transcoder: SOT marker segment expected but %04X is found", word);
      SOT_marker sot(src);
      const uint16_t ti = sot.get_tile_index();
      if (ti >= num_tiles) open_htj2k::detail::fail("transcoder: SOT tile index %u is out of range.", unsigned(ti));
      tin[ti].add_tile_part(sot, src, hin);
    }
  }
  for (uint32_t i = 0; i < num_tiles; ++i) {
    tin[i].line_based_decode = true;
    tin[i].create_tile_buf(hin);
  }

  // Output: the same header with the HT code-block style, one quality layer and a CAP marker
  // segment.  It is parsed from the input again, to be changed without touching the input's.
  j2c_src_memory src2;
  j2k_main_header hout;
  read_main_header(src2, in, in_len, hout);
  const bool reversible = (hout.COD->get_transformation() == 1);
  const auto NL0        = hout.COD->get_dwt_levels();
  hout.COD->set_Cmodes(static_cast<uint8_t>(HT));
  hout.COD->set_number_of_layers(1);
  hout.SIZ->set_cap_capability();
  hout.TLM.clear();
  hout.PLM.clear();
  {
    int32_t B = hout.QCD->get_MAGB(NL0);
    for (const auto &q : hout.QCC) B = std::max<int32_t>(B, q->get_MAGB(NL0));
    const auto bit5 = static_cast<uint16_t>(reversible ? 0 : 1);  // HTREV / HTIRV
    hout.CAP       = std::unique_ptr<CAP_marker>(new CAP_marker());
    hout.CAP->set_Ccap(static_cast<uint16_t>((bit5 << 5) | magb_field(B)), 15);
  }

  std::vector<int32_t *> empty_buf;
  std::unique_ptr<j2k_tile[]> tout(new j2k_tile[num_tiles]);
  for (uint32_t i = 0; i < num_tiles; ++i) tout[i].enc_init(static_cast<uint16_t>(i), hout, empty_buf, true, true);

  // Code-blocks.  Each block is independent, so they run on the thread pool in fixed chunks; every
  // chunk has its own statistics, summed in chunk order, so the output does not depend on the
  // number of threads.
  {
#ifdef OPENHTJ2K_THREAD
    ThreadPoolLease pool_lease;
    ThreadPool *pool = options.num_threads == 1 ? nullptr : ThreadPool::instance(options.num_threads);
#endif
    for (uint32_t i = 0; i < num_tiles; ++i) {
      std::vector<std::pair<j2k_codeblock *, j2k_codeblock *>> pairs;
      for_each_block_pair(tin[i], tout[i],
                          [&](j2k_codeblock *bi, j2k_codeblock *bo) { pairs.emplace_back(bi, bo); });
      constexpr size_t CHUNK = 8;
      const size_t nchunks   = (pairs.size() + CHUNK - 1) / CHUNK;
      std::vector<transcode_stats> cstats(nchunks);
      std::atomic<bool> failed{false};
      std::exception_ptr error;
      std::mutex error_mutex;
      auto run_chunk = [&](size_t k) {
        if (failed.load(std::memory_order_relaxed)) return;
        thread_local scratch_t sc;
        try {
          const size_t end = std::min(pairs.size(), (k + 1) * CHUNK);
          for (size_t j = k * CHUNK; j < end; ++j)
            transcode_block(pairs[j].first, pairs[j].second, sc, options, cstats[k]);
        } catch (...) {
          std::lock_guard<std::mutex> lk(error_mutex);
          if (!error) error = std::current_exception();
          failed.store(true);
        }
      };
      bool done = false;
#ifdef OPENHTJ2K_THREAD
      if (pool != nullptr && pool->num_threads() > 1 && nchunks > 1) {
        std::atomic<size_t> next{0};
        std::atomic<int> running{0};
        auto drain = [&]() {
          for (size_t k = next.fetch_add(1); k < nchunks; k = next.fetch_add(1)) run_chunk(k);
        };
        const int workers = static_cast<int>(std::min<size_t>(pool->num_threads(), nchunks));
        running.store(workers, std::memory_order_relaxed);
        for (int w = 0; w < workers; ++w) {
          pool->push([&]() {
            drain();
            running.fetch_sub(1, std::memory_order_release);
          });
        }
        drain();
        while (running.load(std::memory_order_acquire) > 0) {
          if (!pool->try_run_one()) std::this_thread::yield();
        }
        done = true;
      }
#endif
      if (!done) {
        for (size_t k = 0; k < nchunks; ++k) run_chunk(k);
      }
      if (error) std::rethrow_exception(error);
      for (const transcode_stats &cs : cstats) {
        stats.blocks += cs.blocks;
        stats.full += cs.full;
        stats.coarser += cs.coarser;
        stats.partial += cs.partial;
        stats.refined += cs.refined;
      }
      tout[i].build_packet_headers();
    }
  }

  // The magnitude bound is known now.
  {
    const uint16_t bit5 = static_cast<uint16_t>(reversible ? 0 : 1);
    hout.CAP->set_Ccap(static_cast<uint16_t>((bit5 << 5) | magb_field(coded_magb(tout.get(), num_tiles))), 15);
  }
  if (num_tiles > 1) {
    std::vector<uint16_t> tile_indices(num_tiles);
    std::vector<uint32_t> tile_lengths(num_tiles);
    for (uint32_t i = 0; i < num_tiles; ++i) {
      tile_indices[i] = static_cast<uint16_t>(i);
      tile_lengths[i] = tout[i].get_tile_part_size();
    }
    hout.TLM.push_back(std::unique_ptr<TLM_marker>(new TLM_marker(static_cast<uint8_t>(0), tile_indices, tile_lengths)));
  }

  // SOC, main header, tile-parts, EOC.
  j2c_dst_memory hdr_buf;
  hdr_buf.put_word(_SOC);
  hout.flush(hdr_buf);
  size_t total = hdr_buf.get_length();
  for (uint32_t i = 0; i < num_tiles; ++i) total += tout[i].get_tile_part_size();
  total += 2;
  std::vector<uint8_t> hdr_bytes;
  hdr_buf.flush(&hdr_bytes);

  // JPH output: the boxes in front of the codestream and after it.
  std::vector<uint8_t> file_pre, file_post;
  if (options.jph_output) {
    if (is_file) {
      j2c_dst_memory sig_buf;
      signature_box sig;
      file_type_box ftyp(1);  // brand 'jph '
      sig.write(sig_buf);
      ftyp.write(sig_buf);
      sig_buf.flush(&file_pre);
      std::vector<uint8_t> mid;
      collect_file_boxes(file_in, file_len, mid, file_post);
      file_pre.insert(file_pre.end(), mid.begin(), mid.end());
      uint8_t jp2c[16];
      const size_t n = jp2c_box_header(total, jp2c);
      file_pre.insert(file_pre.end(), jp2c, jp2c + n);
    } else {
      // A raw codestream: boxes made as the encoder makes them; sRGB for three or more
      // components, grayscale otherwise.
      j2c_dst_memory box_buf;
      jph_boxes boxes(hout, 1, hout.SIZ->get_num_components() >= 3, total);
      boxes.write(box_buf);
      box_buf.flush(&file_pre);
    }
  }
  const size_t total_cs = total;
  total += file_pre.size() + file_post.size();
  out.resize(total);
  std::memcpy(out.data(), file_pre.data(), file_pre.size());
  uint8_t *p = out.data() + file_pre.size();
  std::memcpy(p, hdr_bytes.data(), hdr_bytes.size());
  p += hdr_bytes.size();
  for (uint32_t i = 0; i < num_tiles; ++i) p += tout[i].write_packets_direct(hout, p);
  p[0] = 0xFF;
  p[1] = 0xD9;
  p += 2;
  std::memcpy(p, file_post.data(), file_post.size());
  (void)total_cs;
  if (options.stats != nullptr) *options.stats = stats;
  return total;
}

}  // namespace open_htj2k

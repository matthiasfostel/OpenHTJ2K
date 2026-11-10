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

// open_htj2k_transcode (EXPERIMENTAL): Part 1 (EBCOT/MQ) codestream -> HTJ2K codestream.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include "../print_library_messages.hpp"
#include "transcoder.hpp"

static void usage(const char *prog) {
  std::printf(
      "Usage: %s -i <input.j2c> -o <output.j2c>\n"
      "  Transcodes a JPEG 2000 Part 1 (EBCOT/MQ) codestream into an HTJ2K (Part 15)\n"
      "  codestream with the same quantisation indices (EXPERIMENTAL).\n"
      "  -i  input raw codestream (.j2c / .j2k)\n"
      "  -o  output raw codestream (.j2c / .jhc)\n"
      "  -partial keep|drop  a code-block that the Part 1 encoder cut part-way through a\n"
      "      bit-plane: keep = code every decoded bit (default; reconstruction is off by half a\n"
      "      step, and the output is larger), drop = code the block at its last complete cleanup\n"
      "      plane and drop the partial plane. Blocks cut after a cleanup pass are exact either way.\n"
      "  -h  this help\n",
      prog);
}

int main(int argc, char *argv[]) {
  open_htj2k_apps::install_library_message_printer();
  std::string in_name, out_name;
  open_htj2k::transcode_options opt;
  open_htj2k::transcode_stats stats;
  opt.stats = &stats;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
      usage(argv[0]);
      return EXIT_SUCCESS;
    } else if (std::strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
      in_name = argv[++i];
    } else if (std::strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
      out_name = argv[++i];
    } else if (std::strcmp(argv[i], "-partial") == 0 && i + 1 < argc) {
      const char *v = argv[++i];
      if (std::strcmp(v, "keep") == 0) {
        opt.partial_plane = open_htj2k::transcode_partial_plane::keep;
      } else if (std::strcmp(v, "drop") == 0) {
        opt.partial_plane = open_htj2k::transcode_partial_plane::drop;
      } else {
        std::printf("ERROR: -partial takes keep or drop, not %s\n", v);
        return EXIT_FAILURE;
      }
    } else {
      std::printf("ERROR: unknown or incomplete option %s\n", argv[i]);
      usage(argv[0]);
      return EXIT_FAILURE;
    }
  }
  if (in_name.empty() || out_name.empty()) {
    usage(argv[0]);
    return EXIT_FAILURE;
  }

  std::ifstream ifs(in_name, std::ios::in | std::ios::binary);
  if (!ifs) {
    std::printf("ERROR: cannot open %s\n", in_name.c_str());
    return EXIT_FAILURE;
  }
  std::vector<uint8_t> in((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

  std::vector<uint8_t> out;
  const auto t0 = std::chrono::high_resolution_clock::now();
  try {
    open_htj2k::transcode_part1_to_htj2k(in.data(), in.size(), out, opt);
  } catch (std::exception &exc) {
    std::printf("ERROR: transcoding failed: %s\n", exc.what());
    return EXIT_FAILURE;
  }
  const auto t1 = std::chrono::high_resolution_clock::now();

  std::ofstream ofs(out_name, std::ios::out | std::ios::binary);
  if (!ofs) {
    std::printf("ERROR: cannot create %s\n", out_name.c_str());
    return EXIT_FAILURE;
  }
  ofs.write(reinterpret_cast<const char *>(out.data()), static_cast<std::streamsize>(out.size()));
  ofs.close();

  std::printf("Input (Part 1)   = %zu bytes\nOutput (HTJ2K)   = %zu bytes (%.1f%% of input)\n", in.size(),
              out.size(), 100.0 * static_cast<double>(out.size()) / static_cast<double>(in.size()));
  std::printf("Code-blocks with passes = %llu: %llu full, %llu cut after a cleanup pass above the LSB, %llu cut "
              "part-way through a bit-plane\n",
              static_cast<unsigned long long>(stats.blocks), static_cast<unsigned long long>(stats.full),
              static_cast<unsigned long long>(stats.coarser), static_cast<unsigned long long>(stats.partial));
  std::printf("elapsed time %.3f [ms]\n",
              std::chrono::duration<double, std::milli>(t1 - t0).count());
  return EXIT_SUCCESS;
}

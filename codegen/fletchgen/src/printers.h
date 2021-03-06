// Copyright 2018 Delft University of Technology
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

#pragma once

#include <arrow/type.h>

#include "fletcher-streams.h"

namespace fletchgen {

/**
 * Print some info about a field.
 * @param field The field
 * @param parent Any parent stream
 * @return A string with some info about a field.
 */
std::string getFieldInfoString(arrow::Field *field, ArrowStream *parent);

/**
 * Structure for hex editor style command-line output
 */
struct HexView {
  /**
   * @brief Construct a new HexView object
   * @param start Start address of the first byte.
   * @param str Optional string to append any output to.
   * @param row Starting row
   * @param col Starting column
   * @param width Number of bytes per line
   */
  explicit HexView(unsigned long start,
                   std::string str = "",
                   unsigned long row = 0,
                   unsigned long col = 0,
                   unsigned long width = 32);

  ///@brief Return a hex editor style view of the memory that was added to this HexView, optionally with a \p header.
  std::string toString(bool header = true);

  /**
   * @brief Add a memory region to be printed to the HexView
   * @param ptr The memory
   * @param size The size
   */
  void addData(const uint8_t *ptr, size_t size);

  std::string str;
  unsigned long row;
  unsigned long col;
  unsigned long width;
  unsigned long start;
};

}//namespace fletchgen
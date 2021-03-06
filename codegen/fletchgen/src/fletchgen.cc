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

#include <iostream>
#include <sstream>
#include <memory>
#include <fstream>

#include <boost/program_options.hpp>

#include <arrow/api.h>
#include <arrow/ipc/api.h>
#include <arrow/io/api.h>

#include "arrow-utils.h"
#include "column.h"
#include "column-wrapper.h"

#include "srec/recordbatch.h"
#include "vhdt/vhdt.h"
#include "top/axi.h"
#include "schema_test.h"

#include <plasma/client.h>

using fletchgen::Mode;

int main(int argc, char **argv) {
  // TODO: Move this stuff to tests
  //auto schema = genStructSchema();
  //auto schema = genBigSchema();
  //auto schema = genPairHMMSchema();
  //auto schema = genSimpleReadSchema();
  auto schema = genStringSchema();
  fletchgen::writeSchemaToFile(schema, "test.fbs");
  auto rb = getStringRB();
  fletchgen::srec::writeRecordBatchToFile(*rb, "test.rb");

  std::string schema_fname;
  std::string output_fname;
  std::string acc_name;
  std::string wrap_name;
  int regs = 0;

  /* Parse command-line options: */
  namespace po = boost::program_options;
  po::options_description desc("Options");

  desc.add_options()
      ("help,h", "Produce this help message")
      ("input,i", po::value<std::string>(), "Flatbuffer file with Arrow schema to base wrapper on.")
      ("output,o", po::value<std::string>(), "Wrapper output file.")
      ("name,n", po::value<std::string>()->default_value("<input file name>"), "Name of the accelerator component.")
      ("wrapper_name,w", po::value<std::string>()->default_value("fletcher_wrapper"), "Name of the wrapper component.")
      ("custom_registers,r", po::value<int>(), "Number 32-bit registers in accelerator component.")
      ("recordbatch_data,d", po::value<std::string>(), "RecordBatch data input file name for SREC generation.")
      ("recordbatch_schema,s", po::value<std::string>(), "RecordBatch schema input file name for SREC generation.")
      ("srec_output,x", po::value<std::string>(),
         "SREC output file name. If this and recordbatch_in are specified, this "
         "tool will convert an Arrow RecordBatch message stored in a file into an "
         "SREC file. The SREC file can be used in simulation.")
      ("quiet,q", "Prevent output on stdout.")
      ("axi,a", po::value<std::string>()->default_value("axi_top.vhd"), "Generate AXI top level.");

  /* Positional options: */
  po::positional_options_description p;
  p.add("input", -1);

  po::variables_map vm;

  /* Parse command line options with Boost */
  try {
    po::store(po::command_line_parser(argc, argv).options(desc).positional(p).run(), vm);
    po::notify(vm);
  } catch (const po::unknown_option &e) {
    std::cerr << "Unkown option: " << e.get_option_name() << std::endl;
    desc.print(std::cerr);
    return 0;
  }

  /* Option processing: */
  // Help:
  if (vm.count("help")) {
    desc.print(std::cout);
    return 0;
  }

  // Optional RecordBatch <-> SREC conversion
  auto cnt = vm.count("recordbatch_data") + vm.count("recordbatch_schema") + vm.count("srec_output");
  if ((cnt > 1) && (cnt < 3)) {
    std::cout << "Options recordbatch_data, recordbatch_schema and srec_output must all be set. Exiting."
              << std::endl;
    desc.print(std::cout);
    return 0;
  }
  if (vm.count("recordbatch_data")) {
    auto rbd_fname = vm["recordbatch_data"].as<std::string>();
    if (vm.count("recordbatch_schema")) {
      auto rbs_fname = vm["recordbatch_schema"].as<std::string>();
      if (vm.count("srec_output")) {
        auto sro_fname = vm["srec_output"].as<std::string>();
        auto rbs = fletchgen::readSchemaFromFile(rbs_fname);
        auto rbd = fletchgen::srec::readRecordBatchFromFile(rbd_fname, rbs);
        fletchgen::srec::writeRecordBatchToSREC(rbd.get(), sro_fname);
      }
    }
  }
  // Schema input:
  if (vm.count("input")) {
    schema_fname = vm["input"].as<std::string>();
  } else {
    std::cout << "No valid input file specified. Exiting..." << std::endl;
    desc.print(std::cout);
    return 0;
  }

  // VHDL output:
  if (vm.count("output")) {
    output_fname = vm["output"].as<std::string>();
  }

  // UserCore name:
  if (vm["name"].as<std::string>() != "<input file name>") {
    acc_name = vm["name"].as<std::string>();
  } else {
    size_t lastindex = schema_fname.find_last_of('.');
    acc_name = schema_fname.substr(0, lastindex);
  }

  // Wrapper name:
  if (vm.count("wrapper_name")) {
    wrap_name = vm["wrapper_name"].as<std::string>();
  } else {
    wrap_name = nameFrom({"fletcher", "wrapper"});
  }

  // UserCore registers:
  if (vm.count("custom_registers")) {
    regs = vm["custom_registers"].as<int>();
  }

  std::vector<std::ostream *> outputs;

  /* Determine output streams */
  if (vm.count("quiet") == 0) {
    outputs.push_back(&std::cout);
  }

  std::ofstream ofs;
  /* Generate wrapper: */
  if (!output_fname.empty()) {
    ofs = std::ofstream(output_fname);
    outputs.push_back(&ofs);
  }

  auto wrapper = fletchgen::generateColumnWrapper(outputs, fletchgen::readSchemaFromFile(schema_fname), acc_name, wrap_name, regs);
  LOGD("Wrapper generation finished.");

  /* AXI top level */
  if (vm.count("axi")) {
    auto axi_file = vm["axi"].as<std::string>();
    std::ofstream aofs(axi_file);
    std::vector<std::ostream *> axi_outputs = {&aofs};
    if (vm.count("quiet") == 0) {
      axi_outputs.push_back(&std::cout);
    }
    axi::generateAXITop(wrapper, axi_outputs);

  }

  return 0;
}


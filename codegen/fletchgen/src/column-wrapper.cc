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

#include <utility>
#include "column-wrapper.h"
#include "arbiter.h"

using std::string;
using std::make_shared;

using vhdl::seperator;
using vhdl::t;
using vhdl::Range;
using vhdl::Connection;
using vhdl::Generic;

namespace fletchgen {

ColumnWrapper::ColumnWrapper(std::shared_ptr<arrow::Schema> schema, string name, string acc_name, int num_user_regs)
    : StreamComponent(std::move(name)), schema_(std::move(schema)), user_regs_(num_user_regs) {

  /* Generics */
  addGenerics();

  /* Column(Readers/Writers) */
  addColumns(createColumns());

  /* UserCore controller */
  uctrl_ = make_shared<UserCoreController>();
  uctrl_inst_ = make_shared<Instantiation>(nameFrom({uctrl_->entity()->name(), "inst"}),
                                           std::static_pointer_cast<Component>(uctrl_));
  //architecture()->addComponent(uctrl_);
  architecture()->addInstantiation(uctrl_inst_);
  uctrl_inst_->setComment(t(1) + "-- Controller instance.\n");

  /* UserCore */
  usercore_ = make_shared<UserCore>(acc_name, this, countBuffers(), num_user_regs);
  usercore_inst_ = make_shared<Instantiation>(nameFrom({usercore_->entity()->name(), "inst"}),
                                              std::static_pointer_cast<Component>(usercore_));
  architecture()->addComponent(usercore_);
  architecture()->addInstantiation(usercore_inst_);
  usercore_inst_->setComment(t(1) + "-- Hardware Accelerated Function instance.\n");

  /* Arbiter */
  addArbiter();

  /* Ports */
  addGlobalPorts();
  addStreamPorts(&pgroup_);
  addRegisterPorts();

  /* Internal signals */
  addInternalColumnSignals();

  addInternalArbiterSignals(arbiter_->slv_rreq()->ports());
  addInternalArbiterSignals(arbiter_->slv_rdat()->ports());

  implementUserRegs();
  mapUserGenerics();

  addControllerSignals();

  /* Internal connections */
  connectUserCoreStreams();
  connectGlobalPorts();
  connectReadRequestChannels();
  connectReadDataChannels();
  connectControllerRegs();
  connectControllerSignals();
}

int ColumnWrapper::countBuffers() {
  // Count number of buffers
  int bufs = 0;
  for (const auto &c : column_instances()) {
    bufs = bufs + (int) c->getBuffers().size();
  }
  return bufs;
}

void ColumnWrapper::addArbiter() {
  // TODO: don't create arbiter if there is just one column or bypass in arbiter implementation?
  auto num_read_columns = countColumnsOfMode(Mode::READ);
  arbiter_ = make_shared<ReadArbiter>(num_read_columns);
  arbiter_inst_ = make_shared<Instantiation>(nameFrom({arbiter_->entity()->name(), "inst"}),
                                             std::static_pointer_cast<Component>(arbiter_));
  architecture()->addInstantiation(arbiter_inst_);
  arbiter_inst_->mapGeneric(arbiter_->entity()->getGenericByName("NUM_SLAVE_PORTS"), Value(num_read_columns));
  arbiter_inst_
      ->mapGeneric(arbiter_->entity()->getGenericByName(ce::BUS_ADDR_WIDTH), Value(ce::BUS_ADDR_WIDTH));
  arbiter_inst_
      ->mapGeneric(arbiter_->entity()->getGenericByName(ce::BUS_DATA_WIDTH), Value(ce::BUS_DATA_WIDTH));
  arbiter_inst_
      ->mapGeneric(arbiter_->entity()->getGenericByName(ce::BUS_LEN_WIDTH), Value(ce::BUS_LEN_WIDTH));
  arbiter_->mst_rreq()->setSource(arbiter_inst_.get());
  arbiter_inst_->setComment(
      t(1) + "-- Arbiter instance generated to serve " + std::to_string(num_read_columns) + " column readers.\n"
  );
  for (const auto &p : arbiter_->mst_rreq()->ports()) {
    arbiter_inst_->mapPort(p.get(), p.get());
  }
  for (const auto &p : arbiter_->mst_rdat()->ports()) {
    arbiter_inst_->mapPort(p.get(), p.get());
  }

  arbiter_->mst_rreq()->setGroup(pgroup_++);
  arbiter_->mst_rdat()->setGroup(pgroup_++);
  appendStream(arbiter_->mst_rreq());
  appendStream(arbiter_->mst_rdat());
}

void ColumnWrapper::addGenerics() {
  int group = 0;
  entity()->addGeneric(std::make_shared<Generic>(ce::BUS_DATA_WIDTH,
                                                 "natural",
                                                 Value(ce::BUS_DATA_WIDTH_DEFAULT),
                                                 group));
  entity()->addGeneric(std::make_shared<Generic>(ce::BUS_ADDR_WIDTH,
                                                 "natural",
                                                 Value(ce::BUS_ADDR_WIDTH_DEFAULT),
                                                 group));
  entity()->addGeneric(std::make_shared<Generic>(ce::BUS_LEN_WIDTH, "natural", Value(8), group));
  entity()->addGeneric(std::make_shared<Generic>(ce::BUS_BURST_STEP_LEN, "natural", Value(1), group));
  entity()->addGeneric(std::make_shared<Generic>(ce::BUS_BURST_MAX_LEN, "natural", Value(8), group));

  group++;
  entity()->addGeneric(std::make_shared<Generic>(ce::INDEX_WIDTH, "natural", Value(32), group));
  group++;
  entity()->addGeneric(std::make_shared<Generic>("NUM_ARROW_BUFFERS", "natural", Value(countBuffers()), group));
  entity()->addGeneric(std::make_shared<Generic>("NUM_REGS", "natural", Value(countRegisters()), group));
  entity()->addGeneric(std::make_shared<Generic>(ce::NUM_USER_REGS, "natural", Value(user_regs_), group));
  entity()
      ->addGeneric(std::make_shared<Generic>(ce::REG_WIDTH, "natural", Value(ce::REG_WIDTH_DEFAULT), group));
  group++;
  entity()
      ->addGeneric(std::make_shared<Generic>(ce::TAG_WIDTH, "natural", Value(ce::TAG_WIDTH_DEFAULT), group));
}

void ColumnWrapper::addGlobalPorts() {
  LOGD("Generating global ports.");

  // Accelerator clock
  auto aclk = make_shared<GeneralPort>(ce::ACC_CLK, GP::ACC_CLK, Dir::IN);
  auto areset = make_shared<GeneralPort>(ce::ACC_RST, GP::ACC_RESET, Dir::IN);
  // Bus clock
  auto bclk = make_shared<GeneralPort>(ce::BUS_CLK, GP::BUS_CLK, Dir::IN);
  auto breset = make_shared<GeneralPort>(ce::BUS_RST, GP::BUS_RESET, Dir::IN);

  entity()->addPort(aclk, pgroup_);
  entity()->addPort(areset, pgroup_);
  entity()->addPort(bclk, pgroup_);
  entity()->addPort(breset, pgroup_);
  pgroup_++;
}

void ColumnWrapper::addRegisterPorts() {
  // Register file
  auto rin = make_shared<GeneralPort>("regs_in", GP::REG, Dir::IN, Value("NUM_REGS") * Value(ce::REG_WIDTH));
  auto rout = make_shared<GeneralPort>("regs_out", GP::REG, Dir::OUT, Value("NUM_REGS") * Value(ce::REG_WIDTH));
  auto ren = make_shared<GeneralPort>("regs_out_en", GP::REG, Dir::OUT, Value("NUM_REGS"));
  entity()->addPort(rin, pgroup_);
  entity()->addPort(rout, pgroup_);
  entity()->addPort(ren, pgroup_);
  pgroup_++;
}

std::vector<std::shared_ptr<Column>> ColumnWrapper::createColumns() {
  LOGD("Creating Column(Reader/Writer) instances.");

  std::vector<std::shared_ptr<Column>> columns;
  for (int f = 0; f < schema_->num_fields(); f++) {
    auto field = schema_->field(f);
    if (!mustIgnore(field.get())) {
      LOGD("Creating column for [FIELD: " + field->ToString() + "]");
      auto column = make_shared<Column>(field, getMode(schema_.get()));
      // Generate a comment to place above the instantiation
      column->setComment(
          t(1) + "-- " + column->component()->entity()->name() + " instance generated from Arrow schema field:\n" +
          t(1) + "-- " + field->ToString() + "\n"
      );
      columns.push_back(column);
    } else {
      LOGD("Ignoring field " + field->name());
    }
  }
  return columns;
}

void ColumnWrapper::addColumns(const std::vector<std::shared_ptr<Column>> &columns) {
  for (auto const &column : columns) {
    LOGD("Adding instantiation of Column" + getModeString(column->mode()) + ": " + column->name());
    architecture()->addInstantiation(std::static_pointer_cast<Instantiation>(column));
    //CMD_TAG_ENABLE            : boolean := false;
    //CMD_TAG_WIDTH             : natural := 1
  }
}

void ColumnWrapper::addUserStreams() {
  for (const auto &c: column_instances()) {
    // Only Arrow streams should be copied over to the ColumnWrapper.
    auto uds = c->getArrowStreams();
    _streams.insert(std::end(_streams), std::begin(uds), std::end(uds));

    // Append the UserCommandStream for each column
    appendStream(c->generateUserCommandStream());
  }
}

std::shared_ptr<Instantiation> ColumnWrapper::read_arbiter_inst() {
  // Iterate over all component instantiations
  for (auto &inst : architecture()->instances()) {
    // Get only instantiations of Column
    auto ra = std::dynamic_pointer_cast<ReadArbiter>(inst->component());
    if (ra != nullptr)
      return inst;
  }
  LOGD("ReadArbiter was not instantiated in ColumnWrapper architecture.");
  return nullptr;
}

std::vector<std::shared_ptr<Column>> ColumnWrapper::column_instances() {
  std::vector<std::shared_ptr<Column>> ci;

  // Iterate over all component instantiations
  for (auto &inst : architecture()->instances()) {
    // Get only instantiations of Column
    auto column = std::dynamic_pointer_cast<Column>(inst);
    if (column != nullptr)
      ci.push_back(column);
  }
  return ci;
}

string ColumnWrapper::toString() { return "[WRAPPER: " + entity_->name() + "]"; }

string ColumnWrapper::toInfoString() {
  string ret;
  ret += "[WRAPPER: " + entity_->name() + "]\n";
  ret += t(1) + "Streams: \n";
  for (const auto &s : streams()) {
    ret += t(2) + s->toString() + "\n";
  }
  ret += t(1) + "Column(Readers/Writers): \n";
  for (const auto &c : column_instances()) {
    ret += t(2) + c->toString() + "\n";
  }
  return ret;
}

int ColumnWrapper::countColumnsOfMode(Mode mode) {
  int count = 0;
  for (const auto &i : column_instances()) {
    if (i->mode() == mode) {
      count++;
    }
  }
  return count;
}

std::vector<FletcherStream *> ColumnWrapper::getFletcherStreams(std::vector<std::shared_ptr<Stream>> streams) {
  std::vector<FletcherStream *> ret;
  for (const auto &s: streams) {
    auto fcs = dynamic_cast<FletcherStream *>(s.get());
    if (fcs != nullptr) {
      ret.push_back(fcs);
    }
  }
  return ret;
}

void ColumnWrapper::addInternalColumnSignals() {
  LOGD("Adding wrapper internal column signals.");
  // From each column instance
  string comment;
  for (const auto &i: column_instances()) {
    auto ports = i->component()->entity()->ports();
    for (const auto &p: ports) {
      std::shared_ptr<SignalFromPort> sig;

      // First make signals for the ArrowPorts.
      auto ap = dynamic_cast<GeneralPort *>(p);
      if (ap == nullptr) {
        // StreamPorts
        auto name = nameFrom({vhdl::INT_SIG, i->field()->name(), p->name()});
        if (p->isVector()) {
          sig = std::make_shared<SignalFromPort>(name, p->width(), p);
        } else {
          sig = std::make_shared<SignalFromPort>(name, p);
        }
      } else {
        // Any other ports?
      }

      // If a signal was created, add it to the architecture.
      if (sig != nullptr) {
        // Group the signals by Column instance
        sig->setGroup(sgroup_);
        architecture()->addSignal(sig, sgroup_);
        // Map the port on the instatiation to the wrapper signal.
        i->mapPort(p, sig.get());
      }
    }
    sgroup_++;
  }
}

void ColumnWrapper::addInternalArbiterSignals(std::vector<std::shared_ptr<StreamPort>> ports) {
  // ReadArbiter (if any)
  auto rai = read_arbiter_inst();
  if (rai != nullptr) {
    auto ra = std::dynamic_pointer_cast<ReadArbiter>(rai->component());
    for (const auto &p : ports) {
      auto name = nameFrom({vhdl::INT_SIG, p->name()});
      auto sig = std::make_shared<SignalFromPort>(name, p->width(), p.get());
      architecture()->addSignal(sig, sgroup_);
      rai->mapPort(p.get(), sig.get());
    }
    sgroup_++;
  }
}

int ColumnWrapper::countRegisters() {
  int reg_count = 0;

  // Assuming 32-bit registers:
  reg_count += 1;                  // Status
  reg_count += 1;                  // Control
  reg_count += 2;                  // Return value (64 bit to support addresses)
  reg_count += 1;                  // First index
  reg_count += 1;                  // Last index
  reg_count += 2 * countBuffers(); // Buffer addresses
  reg_count += user_regs_;         // User registers.

  return reg_count;
}

void ColumnWrapper::connectReadRequestChannels() {
  // First find all the column bus signals
  int offset = 0;
  for (const auto &c : column_instances()) {
    if (c->mode() == Mode::READ) {
      auto cr = std::static_pointer_cast<ColumnReader>(c->component());
      auto ports = cr->stream_rreq_->ports();
      for (const auto &p : ports) {
        auto rrp = std::dynamic_pointer_cast<ReadReqPort>(p);
        auto rrs = dynamic_cast<ReadRequestStream *>(p->parent());
        if (rrs != nullptr) {
          auto sname = rrs->name();
          auto col_signame = nameFrom({vhdl::INT_SIG, c->field()->name(), p->name()});
          auto col_sig = architecture()->getSignal(col_signame);
          auto arb_signame = nameFrom({vhdl::INT_SIG, "bsv", typeToString(rrs->type()), typeToString(rrp->type())});
          auto arb_sig = architecture()->getSignal(arb_signame);

          Range range;

          if (!col_sig->isVector()) {
            range = Range(Value(offset), Value(offset), Range::Type::SINGLE);
          } else {
            auto hi = col_sig->width() * (offset + 1) - Value(1);
            auto lo = col_sig->width() * offset;
            range = Range(hi, lo);
          }
          auto invert = rrp->dir() == Dir::OUT;
          auto con = std::make_shared<Connection>(col_sig, Range(), arb_sig, range, invert);
          con->setGroup(pgroup_);
          architecture()->addConnection(con);
        }
      }
    }
    offset++;
    pgroup_++;
  }
}

void ColumnWrapper::connectReadDataChannels() {
  // First find all the column bus signals
  int offset = 0;
  for (const auto &c : column_instances()) {
    if (c->mode() == Mode::READ) {
      auto cr = std::static_pointer_cast<ColumnReader>(c->component());
      for (const auto &p : cr->stream_rdat_->ports()) {
        auto rdp = std::dynamic_pointer_cast<ReadDataPort>(p);
        auto rds = dynamic_cast<ReadDataStream *>(p->parent());
        auto sname = rds->name();
        auto col_signame = nameFrom({vhdl::INT_SIG, c->field()->name(), p->name()});
        auto col_sig = architecture()->getSignal(col_signame);
        auto arb_signame = nameFrom({vhdl::INT_SIG, "bsv", typeToString(rds->type()), typeToString(rdp->type())});
        auto arb_sig = architecture()->getSignal(arb_signame);

        Range range;

        if (!col_sig->isVector()) {
          range = Range(Value(offset), Value(offset), Range::Type::SINGLE);
        } else {
          auto hi = col_sig->width() * (offset + 1) - Value(1);
          auto lo = col_sig->width() * offset;
          range = Range(hi, lo);
        }

        auto invert = rdp->dir() == Dir::OUT;
        auto con = std::make_shared<Connection>(col_sig, Range(), arb_sig, range, invert);
        con->setGroup(pgroup_);
        architecture()->addConnection(con);
      }
    }
    offset++;
    pgroup_++;
  }
}

void ColumnWrapper::connectUserCoreStreams() {
  // We have to connect the UserCore streams to the signals that were derived from the columns.
  LOGD("Connecting internal wrapper signals to User Core stream ports.");
  for (const auto &generic_stream : usercore_->streams()) {
    auto stream = dynamic_cast<FletcherColumnStream *>(generic_stream.get());
    if (stream != nullptr) {
      auto column = stream->source();
      LOGD(stream->toString());
      LOGD("  Derived from: " + stream->source()->toString());
      LOGD("  Connections : ");
      // For each port on the UserCore stream
      for (const auto &stream_port: stream->ports()) {
        // If it's an Arrow port:
        auto arrow_port = dynamic_cast<ArrowPort *>(stream_port.get());
        if (arrow_port != nullptr) {
          connectArrowPortToSignal(stream, column, arrow_port);
        }
        // If it's a Command port:
        auto command_port = dynamic_cast<CommandPort *>(stream_port.get());
        if (command_port != nullptr) {
          connectCommandPortToSignal(stream, column, command_port);
        }
      }
      pgroup_++;
    }
  }
}

void ColumnWrapper::connectArrowPortToSignal(FletcherColumnStream *stream,
                                             Column *column,
                                             ArrowPort *port) {
  // Derive the name of the signal on this wrapper from the stream port.
  auto signame = nameFrom(
      {vhdl::INT_SIG, column->field()->name(), typeToString(stream->type()),
       typeToString(mapUserTypeToColumnType(port->type()))}
  );
  // Get  the signal
  auto signal = architecture()->getSignal(signame);
  Range range;
  if (!port->isVector() && (port->width() == Value(1))) {
    range = Range(port->offset() + port->width() - Value(1), port->offset(), Range::SINGLE);
  } else {
    range = Range(port->offset() + port->width() - Value(1), port->offset());
  }
  usercore_inst_->mapPort(port, signal, range);
}

void ColumnWrapper::connectCommandPortToSignal(FletcherColumnStream *stream,
                                               Column *column,
                                               CommandPort *port) {
  // Derive the name of the signal on this wrapper from the stream port.
  auto signame = nameFrom(
      {vhdl::INT_SIG, column->field()->name(), typeToString(stream->type()),
       typeToString(mapUserTypeToColumnType(port->type()))}
  );
  // Get  the signal
  auto signal = architecture()->getSignal(signame);
  Range range;
  if (port->width() == Value(1)) {
    range = Range(port->offset() + port->width() - Value(1), port->offset(), Range::SINGLE);
  } else {
    range = Range(port->offset() + port->width() - Value(1), port->offset());
  }
  usercore_inst_->mapPort(port, signal, range);
}

void ColumnWrapper::connectGlobalPortsOf(Instantiation *instance) {
  auto wrap_bus_clk = entity()->getPortByName(ce::BUS_CLK);
  auto wrap_bus_rst = entity()->getPortByName(ce::BUS_RST);
  auto wrap_acc_clk = entity()->getPortByName(ce::ACC_CLK);
  auto wrap_acc_rst = entity()->getPortByName(ce::ACC_RST);

  auto bus_clk = instance->component()->entity()->getPortByName(ce::BUS_CLK);
  auto bus_rst = instance->component()->entity()->getPortByName(ce::BUS_RST);
  auto acc_clk = instance->component()->entity()->getPortByName(ce::ACC_CLK);
  auto acc_rst = instance->component()->entity()->getPortByName(ce::ACC_RST);

  if (bus_clk != nullptr)
    instance->mapPort(bus_clk, wrap_bus_clk);
  if (bus_rst != nullptr)
    instance->mapPort(bus_rst, wrap_bus_rst);
  if (acc_clk != nullptr)
    instance->mapPort(acc_clk, wrap_acc_clk);
  if (acc_rst != nullptr)
    instance->mapPort(acc_rst, wrap_acc_rst);
}

void ColumnWrapper::connectGlobalPorts() {
  // Connect each column
  for (const auto &c: column_instances()) {
    connectGlobalPortsOf(c.get());
  }
  // Connect the rest
  connectGlobalPortsOf(arbiter_inst_.get());
  connectGlobalPortsOf(uctrl_inst_.get());
  connectGlobalPortsOf(usercore_inst_.get());
}

void ColumnWrapper::connectControllerRegs() {
  auto rin = entity()->getPortByName("regs_in");
  auto rout = entity()->getPortByName("regs_out");

  auto cp = uctrl_->entity()->getPortByName("control");
  Range range = Range(Value(ce::REG_WIDTH) - Value(1), Value(0));
  uctrl_inst_->mapPort(cp, rin, range);

  auto sp = uctrl_->entity()->getPortByName("status");
  range = Range(Value(2) * Value(ce::REG_WIDTH) - Value(1), Value(ce::REG_WIDTH));
  uctrl_inst_->mapPort(sp, rout, range);

}

void ColumnWrapper::addControllerSignals() {
  architecture()->addSignalsFromEntityPorts(uctrl_->entity().get(), "uctrl", sgroup_);
  architecture()->removeSignal(nameFrom({"uctrl", ce::BUS_CLK}));
  architecture()->removeSignal(nameFrom({"uctrl", ce::BUS_RST}));
  architecture()->removeSignal(nameFrom({"uctrl", ce::ACC_CLK}));
  architecture()->removeSignal(nameFrom({"uctrl", ce::ACC_RST}));
  sgroup_++;
}

void ColumnWrapper::connectControllerSignals() {
  usercore_inst_->mapPort(usercore_->start(), architecture()->getSignal("uctrl_start"));
  usercore_inst_->mapPort(usercore_->stop(), architecture()->getSignal("uctrl_stop"));
  usercore_inst_->mapPort(usercore_->reset(), architecture()->getSignal("uctrl_reset"));
  usercore_inst_->mapPort(usercore_->idle(), architecture()->getSignal("uctrl_idle"));
  usercore_inst_->mapPort(usercore_->busy(), architecture()->getSignal("uctrl_busy"));
  usercore_inst_->mapPort(usercore_->done(), architecture()->getSignal("uctrl_done"));

  uctrl_inst_->mapPort(uctrl_->start(), architecture()->getSignal("uctrl_start"));
  uctrl_inst_->mapPort(uctrl_->stop(), architecture()->getSignal("uctrl_stop"));
  uctrl_inst_->mapPort(uctrl_->reset(), architecture()->getSignal("uctrl_reset"));
  uctrl_inst_->mapPort(uctrl_->idle(), architecture()->getSignal("uctrl_idle"));
  uctrl_inst_->mapPort(uctrl_->busy(), architecture()->getSignal("uctrl_busy"));
  uctrl_inst_->mapPort(uctrl_->done(), architecture()->getSignal("uctrl_done"));

  auto gen = uctrl_inst_->component()->entity()->getGenericByName(ce::REG_WIDTH);
  uctrl_inst_->mapGeneric(gen, Value(ce::REG_WIDTH));
}

void ColumnWrapper::implementUserRegs() {
  /* User Regs */
  if (user_regs_ > 0) {
    auto srin = architecture()->addSignalFromPort(usercore_->user_regs_in(), "s", sgroup_);
    auto srout = architecture()->addSignalFromPort(usercore_->user_regs_out(), "s", sgroup_);
    auto sroute = architecture()->addSignalFromPort(usercore_->user_regs_out_en(), "s", sgroup_);
    usercore_inst_->mapPort(usercore_->user_regs_in(), srin);
    usercore_inst_->mapPort(usercore_->user_regs_out(), srout);
    usercore_inst_->mapPort(usercore_->user_regs_out_en(), sroute);
    sgroup_++;

    Range rr = Range(Value("NUM_REGS*REG_WIDTH") - Value(1), (Value("(NUM_REGS-NUM_USER_REGS)*REG_WIDTH")));
    Range wer = Range(Value("NUM_REGS") - Value(1), Value("NUM_REGS-NUM_USER_REGS"));

    architecture()->addConnection(make_shared<Connection>(regs_in(), rr, srin, Range()));
    architecture()->addConnection(make_shared<Connection>(srout, Range(), regs_out(), rr));
    architecture()->addConnection(make_shared<Connection>(sroute, Range(), regs_out_en(), wer));
  }

  /* Buffer Address Regs */
  int i = 0;
  for (const auto &b : usercore_->buffers()) {
    Range r(Value((ce::NUM_DEFAULT_REGS + ce::REGS_PER_ADDRESS * (i + 1))) * Value(ce::REG_WIDTH)
            - Value(1),
            Value(ce::NUM_DEFAULT_REGS + (ce::REGS_PER_ADDRESS * i)) * Value(ce::REG_WIDTH));
    auto p = usercore_->entity()->getPortByName(nameFrom({"reg", b->name(), "addr"}));
    usercore_inst_->mapPort(p, regs_in(), r);
    i++;
  }
}

GeneralPort *ColumnWrapper::regs_in() {
  return dynamic_cast<GeneralPort *>(entity()->getPortByName("regs_in"));
}

GeneralPort *ColumnWrapper::regs_out() {
  return dynamic_cast<GeneralPort *>(entity()->getPortByName("regs_out"));
}

GeneralPort *ColumnWrapper::regs_out_en() {
  return dynamic_cast<GeneralPort *>(entity()->getPortByName("regs_out_en"));
}

void ColumnWrapper::mapUserGenerics() {
  auto ent = usercore_->entity();
  if (user_regs_ > 0) {
    usercore_inst_->mapGeneric(ent->getGenericByName(ce::NUM_USER_REGS), Value(ce::NUM_USER_REGS));
  }
  usercore_inst_->mapGeneric(ent->getGenericByName(ce::TAG_WIDTH), Value(ce::TAG_WIDTH));
  usercore_inst_->mapGeneric(ent->getGenericByName(ce::BUS_ADDR_WIDTH), Value(ce::BUS_ADDR_WIDTH));
  usercore_inst_->mapGeneric(ent->getGenericByName(ce::INDEX_WIDTH), Value(ce::INDEX_WIDTH));
  usercore_inst_->mapGeneric(ent->getGenericByName(ce::REG_WIDTH), Value(ce::REG_WIDTH));
}

} //namespace fletchgen
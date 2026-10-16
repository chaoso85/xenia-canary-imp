/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the project root for more  *
 * information.                                                               *
 ******************************************************************************
 */

#ifndef XENIA_CPU_PPC_PPC_GUEST_TRACE_H_
#define XENIA_CPU_PPC_PPC_GUEST_TRACE_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace xe {
class Memory;
namespace cpu {
class Function;
class Processor;
namespace ppc {

// Guest trace probes (--trace_guest_addresses). Each probe is a guest
// instruction address; when the instruction is about to execute, its GPRs,
// FPRs, LR and optional memory views are written to the log at info level
// with a [GTRACE] prefix.
//
// Syntax: comma separated entries
//   ADDR[=NAME][@rN:VIEW]...
// VIEW: s = UTF-16BE string, a = ANSI string, fK = K floats,
//       wK = K big-endian dwords, xK = K bytes as hex.
// Example:
//   922290D8=XuiMeasureText@r4:s,92229250=XuiMeasureText.exit@r31:f4
struct GuestTraceView {
  uint32_t gpr;
  char kind;
  uint32_t count;
};

struct GuestTraceProbe {
  uint32_t address;
  std::string name;
  std::vector<GuestTraceView> views;
  uint32_t hits = 0;
  Function* builtin = nullptr;
  Memory* memory = nullptr;
};

class GuestTrace {
 public:
  void Initialize(Processor* processor, Memory* memory);
  // Builtin to call before executing the instruction at address, or null.
  Function* ProbeFor(uint32_t address) const;

 private:
  std::map<uint32_t, GuestTraceProbe> probes_;
};

}  // namespace ppc
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_PPC_PPC_GUEST_TRACE_H_

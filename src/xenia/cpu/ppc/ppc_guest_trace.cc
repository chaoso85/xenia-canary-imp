/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the project root for more  *
 * information.                                                               *
 ******************************************************************************
 */

#include "xenia/cpu/ppc/ppc_guest_trace.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/base/string.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/memory.h"

DEFINE_string(trace_guest_addresses, "",
              "Guest instruction probes that log registers when reached. "
              "Comma separated ADDR[=NAME][@rN:VIEW]..., VIEW is s (UTF-16 "
              "string), a (ANSI string), fK (K floats), wK (K dwords) or xK "
              "(K bytes). Example: 922290D8=XuiMeasureText@r4:s",
              "CPU");
DEFINE_uint32(trace_guest_max_hits, 500,
              "Maximum number of log lines per guest trace probe.", "CPU");

namespace xe {
namespace cpu {
namespace ppc {

namespace {

bool Readable(Memory* memory, uint32_t address, uint32_t length) {
  if (!address || !length) {
    return false;
  }
  uint64_t end = uint64_t(address) + length - 1;
  if (end > 0xFFFFFFFFull) {
    return false;
  }
  auto heap = memory->LookupHeap(address);
  if (!heap || heap != memory->LookupHeap(uint32_t(end))) {
    return false;
  }
  auto access = heap->QueryRangeAccess(address, uint32_t(end));
  return (uint32_t(access) & uint32_t(xe::memory::PageAccess::kReadOnly)) != 0;
}

std::string FormatView(Memory* memory, uint32_t address,
                       const GuestTraceView& view) {
  switch (view.kind) {
    case 's': {
      std::u16string text;
      for (uint32_t i = 0; i < 256 && Readable(memory, address + i * 2, 2);
           ++i) {
        char16_t c = char16_t(xe::load_and_swap<uint16_t>(
            memory->TranslateVirtual<const uint8_t*>(address + i * 2)));
        if (!c) {
          break;
        }
        text.push_back(c);
      }
      std::string out;
      for (char16_t c : text) {
        if (c >= 0x20 && c < 0x7F) {
          out.push_back(char(c));
        } else {
          out += fmt::format("\\u{:04X}", uint32_t(c));
        }
      }
      return "\"" + out + "\"";
    }
    case 'a': {
      std::string out;
      for (uint32_t i = 0; i < 256 && Readable(memory, address + i, 1); ++i) {
        char c = *memory->TranslateVirtual<const char*>(address + i);
        if (!c) {
          break;
        }
        out.push_back(c);
      }
      return "\"" + out + "\"";
    }
    case 'f':
    case 'w':
    case 'x': {
      uint32_t unit = view.kind == 'x' ? 1 : 4;
      if (!Readable(memory, address, view.count * unit)) {
        return "<unreadable>";
      }
      auto p = memory->TranslateVirtual<const uint8_t*>(address);
      std::string out;
      for (uint32_t i = 0; i < view.count; ++i) {
        if (i) {
          out.push_back(' ');
        }
        if (view.kind == 'x') {
          out += fmt::format("{:02X}", p[i]);
        } else {
          uint32_t v = xe::load_and_swap<uint32_t>(p + i * 4);
          if (view.kind == 'w') {
            out += fmt::format("{:08X}", v);
          } else {
            float f;
            std::memcpy(&f, &v, 4);
            out += fmt::format("{}", f);
          }
        }
      }
      return out;
    }
  }
  return "?";
}

void ProbeHandler(PPCContext* ctx, void* arg0, void* arg1) {
  auto probe = reinterpret_cast<GuestTraceProbe*>(arg0);
  if (probe->hits >= cvars::trace_guest_max_hits) {
    return;
  }
  ++probe->hits;
  std::string line =
      fmt::format("[GTRACE] {:08X} {} #{} lr={:08X}", probe->address,
                  probe->name, probe->hits, uint32_t(ctx->lr));
  for (int i = 3; i <= 10; ++i) {
    line += fmt::format(" r{}={:08X}", i, uint32_t(ctx->r[i]));
  }
  for (int i = 25; i <= 31; ++i) {
    line += fmt::format(" r{}={:08X}", i, uint32_t(ctx->r[i]));
  }
  for (int i = 0; i <= 4; ++i) {
    line += fmt::format(" f{}={}", i, ctx->f[i]);
  }
  for (const auto& view : probe->views) {
    uint32_t address = uint32_t(ctx->r[view.gpr]);
    line += fmt::format(" | r{}[{:08X}]={}", view.gpr, address,
                        FormatView(probe->memory, address, view));
  }
  XELOGI("{}", line);
}

}  // namespace

void GuestTrace::Initialize(Processor* processor, Memory* memory) {
  std::string spec = cvars::trace_guest_addresses;
  size_t pos = 0;
  while (pos < spec.size()) {
    size_t end = spec.find(',', pos);
    if (end == std::string::npos) {
      end = spec.size();
    }
    std::string entry = spec.substr(pos, end - pos);
    pos = end + 1;
    // Trim spaces.
    while (!entry.empty() && entry.front() == ' ') {
      entry.erase(0, 1);
    }
    while (!entry.empty() && entry.back() == ' ') {
      entry.pop_back();
    }
    if (entry.empty()) {
      continue;
    }
    GuestTraceProbe probe;
    size_t at = entry.find('@');
    std::string head = entry.substr(0, at);
    size_t eq = head.find('=');
    probe.address =
        uint32_t(std::strtoul(head.substr(0, eq).c_str(), nullptr, 16));
    probe.name = eq == std::string::npos ? std::string() : head.substr(eq + 1);
    while (at != std::string::npos) {
      size_t next = entry.find('@', at + 1);
      std::string v =
          entry.substr(at + 1, next == std::string::npos ? std::string::npos
                                                         : next - at - 1);
      at = next;
      // rN:K[count]
      size_t colon = v.find(':');
      if (v.size() < 4 || (v[0] != 'r' && v[0] != 'R') ||
          colon == std::string::npos || colon + 1 >= v.size()) {
        continue;
      }
      GuestTraceView view;
      view.gpr =
          uint32_t(std::strtoul(v.substr(1, colon - 1).c_str(), nullptr, 10)) &
          31;
      view.kind = char(std::tolower(uint8_t(v[colon + 1])));
      view.count =
          uint32_t(std::strtoul(v.substr(colon + 2).c_str(), nullptr, 10));
      if (!view.count) {
        view.count = 4;
      }
      view.count = std::min<uint32_t>(view.count, 64);
      probe.views.push_back(view);
    }
    if (!probe.address) {
      continue;
    }
    probe.memory = memory;
    auto& stored = probes_[probe.address];
    stored = std::move(probe);
    stored.builtin = processor->DefineBuiltin(
        fmt::format("GuestTrace_{:08X}", stored.address), ProbeHandler, &stored,
        nullptr);
    XELOGI("Guest trace probe at {:08X} ({}) with {} memory views",
           stored.address, stored.name, stored.views.size());
  }
}

Function* GuestTrace::ProbeFor(uint32_t address) const {
  if (probes_.empty()) {
    return nullptr;
  }
  auto it = probes_.find(address);
  return it == probes_.end() ? nullptr : it->second.builtin;
}

}  // namespace ppc
}  // namespace cpu
}  // namespace xe

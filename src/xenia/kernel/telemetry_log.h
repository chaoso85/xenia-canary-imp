/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the project root for more  *
 * information.                                                               *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_TELEMETRY_LOG_H_
#define XENIA_KERNEL_TELEMETRY_LOG_H_

#include <cstdint>
#include <string>
#include <string_view>

#include "xenia/base/cvar.h"
#include "xenia/cpu/ppc/ppc_context.h"

DECLARE_bool(telemetry_log);
DECLARE_bool(telemetry_capture_xhttp);

namespace xe {
namespace kernel {

// Separate log for everything the guest tries to report back to Microsoft:
// ETX event tracing, web instrumentation reports, data-center logging and
// HTTP requests made through XHttp. Lines are appended to telemetry.log in
// the storage root (or the path given by --telemetry_log_path) and mirrored
// into the main log at info level with a [TELEMETRY] prefix.
void TelemetryLog(std::string_view source, std::string_view message);

// Safe guest memory readers for log formatting. They return an empty string
// (or false) for null or unreadable addresses instead of faulting.
bool TelemetryGuestReadable(uint32_t guest_address, uint32_t length);
std::string TelemetryReadAnsi(uint32_t guest_address, uint32_t max_length);
std::string TelemetryReadUtf16(uint32_t guest_address, uint32_t max_length);
// Guesses between UTF-16BE and ANSI from the first bytes.
std::string TelemetryReadAnyString(uint32_t guest_address, uint32_t max_length);
std::string TelemetryHexDump(uint32_t guest_address, uint32_t length);
std::string TelemetryFormatGuid(uint32_t guest_address);

// Integer argument n (0-based, counting from r3) of the current guest call,
// including arguments spilled to the caller's parameter save area. Used for
// variadic exports such as EtxProducerLog and XamDataCenterLog.
uint64_t TelemetryGuestArg(const cpu::ppc::PPCContext* ctx, uint32_t n);

// Minimal printf for guest format strings (%d %i %u %x %X %p %c %s %S %ls
// %hs, width/flags ignored). Argument n is fetched through get_arg(n).
template <typename GetArg>
std::string TelemetryFormatGuest(std::string_view format, GetArg get_arg);

}  // namespace kernel
}  // namespace xe

// --- template implementation -------------------------------------------------
#include "third_party/fmt/include/fmt/format.h"

namespace xe {
namespace kernel {

template <typename GetArg>
std::string TelemetryFormatGuest(std::string_view format, GetArg get_arg) {
  std::string out;
  uint32_t arg_index = 0;
  for (size_t i = 0; i < format.size(); ++i) {
    char c = format[i];
    if (c != '%') {
      out.push_back(c);
      continue;
    }
    if (++i >= format.size()) {
      break;
    }
    if (format[i] == '%') {
      out.push_back('%');
      continue;
    }
    // Skip flags, width, precision.
    while (i < format.size() &&
           (std::string_view("-+ #0123456789.*").find(format[i]) !=
            std::string_view::npos)) {
      if (format[i] == '*') {
        ++arg_index;
      }
      ++i;
    }
    bool wide = false;
    bool narrow = false;
    bool is_64 = false;
    while (i < format.size()) {
      char m = format[i];
      if (m == 'l') {
        wide = true;
      } else if (m == 'h') {
        narrow = true;
      } else if (m == 'I' && i + 2 < format.size() && format[i + 1] == '6' &&
                 format[i + 2] == '4') {
        is_64 = true;
        i += 2;
      } else if (m != 'z' && m != 'L' && m != 'q') {
        break;
      }
      ++i;
    }
    if (i >= format.size()) {
      break;
    }
    uint64_t value = get_arg(arg_index++);
    if (!is_64) {
      value &= 0xFFFFFFFFull;
    }
    switch (format[i]) {
      case 'd':
      case 'i':
        out += is_64 ? fmt::format("{}", static_cast<int64_t>(value))
                     : fmt::format("{}", static_cast<int32_t>(value));
        break;
      case 'u':
        out += fmt::format("{}", value);
        break;
      case 'x':
        out += fmt::format("{:x}", value);
        break;
      case 'X':
        out += fmt::format("{:X}", value);
        break;
      case 'p':
        out += fmt::format("{:08X}", value);
        break;
      case 'c':
        out.push_back(static_cast<char>(value));
        break;
      case 's':
        out += (wide && !narrow)
                   ? TelemetryReadUtf16(static_cast<uint32_t>(value), 512)
                   : TelemetryReadAnsi(static_cast<uint32_t>(value), 512);
        break;
      case 'S':
        out += narrow
                   ? TelemetryReadAnsi(static_cast<uint32_t>(value), 512)
                   : TelemetryReadAnyString(static_cast<uint32_t>(value), 512);
        break;
      default:
        out.push_back('%');
        out.push_back(format[i]);
        break;
    }
  }
  return out;
}

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_TELEMETRY_LOG_H_

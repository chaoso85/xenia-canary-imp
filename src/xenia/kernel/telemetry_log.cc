/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the project root for more  *
 * information.                                                               *
 ******************************************************************************
 */

#include "xenia/kernel/telemetry_log.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/memory.h"

DEFINE_bool(telemetry_log, true,
            "Write telemetry requests made by the guest (ETX events, web "
            "instrumentation reports, data-center logs, XHttp requests) to a "
            "separate log file.",
            "Logging");
DEFINE_path(telemetry_log_path, "",
            "Path of the telemetry log. Empty means telemetry.log in the "
            "storage root.",
            "Logging");
DEFINE_bool(telemetry_capture_xhttp, false,
            "Hand out XHttp session/connection/request handles so the guest "
            "goes on to build its HTTP requests, which are then written to "
            "the telemetry log. Nothing is sent; every request fails as if "
            "the server could not be reached. Off by default because titles "
            "that previously saw XHttpOpen fail will now take a different "
            "code path.",
            "Logging");

namespace xe {
namespace kernel {

namespace {
std::mutex telemetry_mutex;
FILE* telemetry_file = nullptr;
bool telemetry_open_failed = false;

FILE* GetTelemetryFile() {
  if (telemetry_file || telemetry_open_failed) {
    return telemetry_file;
  }
  std::filesystem::path path = cvars::telemetry_log_path;
  if (path.empty()) {
    auto state = kernel_state();
    if (state && state->emulator()) {
      path = state->emulator()->storage_root() / "telemetry.log";
    } else {
      path = "telemetry.log";
    }
  }
  telemetry_file = xe::filesystem::OpenFile(path, "a");
  if (!telemetry_file) {
    telemetry_open_failed = true;
    XELOGW("Telemetry: could not open {}", xe::path_to_utf8(path));
    return nullptr;
  }
  std::fprintf(telemetry_file, "---- Xenia Improved telemetry session ----\n");
  std::fflush(telemetry_file);
  return telemetry_file;
}

std::string Timestamp() {
  auto now = std::chrono::system_clock::now();
  auto t = std::chrono::system_clock::to_time_t(now);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch())
                .count() %
            1000;
  std::tm tm_local{};
#if XE_PLATFORM_WIN32
  localtime_s(&tm_local, &t);
#else
  localtime_r(&t, &tm_local);
#endif
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm_local);
  return fmt::format("{}.{:03}", buffer, ms);
}
}  // namespace

void TelemetryLog(std::string_view source, std::string_view message) {
  if (!cvars::telemetry_log) {
    return;
  }
  uint32_t title_id = 0;
  if (auto state = kernel_state()) {
    title_id = state->title_id();
  }
  std::string line =
      fmt::format("{} [{:08X}] {}: {}", Timestamp(), title_id, source, message);
  XELOGI("[TELEMETRY] {}: {}", source, message);
  std::lock_guard<std::mutex> lock(telemetry_mutex);
  if (FILE* file = GetTelemetryFile()) {
    std::fprintf(file, "%s\n", line.c_str());
    std::fflush(file);
  }
}

bool TelemetryGuestReadable(uint32_t guest_address, uint32_t length) {
  if (!guest_address || !length) {
    return false;
  }
  uint64_t end = uint64_t(guest_address) + length - 1;
  if (end > 0xFFFFFFFFull) {
    return false;
  }
  auto memory = kernel_memory();
  auto heap = memory->LookupHeap(guest_address);
  if (!heap || heap != memory->LookupHeap(uint32_t(end))) {
    return false;
  }
  auto access = heap->QueryRangeAccess(guest_address, uint32_t(end));
  return (uint32_t(access) & uint32_t(xe::memory::PageAccess::kReadOnly)) != 0;
}

std::string TelemetryReadAnsi(uint32_t guest_address, uint32_t max_length) {
  std::string out;
  for (uint32_t i = 0; i < max_length; ++i) {
    if (!TelemetryGuestReadable(guest_address + i, 1)) {
      break;
    }
    char c = *kernel_memory()->TranslateVirtual<const char*>(guest_address + i);
    if (!c) {
      break;
    }
    out.push_back(c);
  }
  return out;
}

std::string TelemetryReadUtf16(uint32_t guest_address, uint32_t max_length) {
  std::u16string out;
  for (uint32_t i = 0; i < max_length; ++i) {
    uint32_t address = guest_address + i * 2;
    if (!TelemetryGuestReadable(address, 2)) {
      break;
    }
    char16_t c = xe::load_and_swap<char16_t>(
        kernel_memory()->TranslateVirtual<const uint8_t*>(address));
    if (!c) {
      break;
    }
    out.push_back(c);
  }
  return xe::to_utf8(out);
}

std::string TelemetryReadAnyString(uint32_t guest_address,
                                   uint32_t max_length) {
  if (!TelemetryGuestReadable(guest_address, 2)) {
    return std::string();
  }
  auto p = kernel_memory()->TranslateVirtual<const uint8_t*>(guest_address);
  // UTF-16BE ASCII text starts with a zero high byte.
  if (p[0] == 0 && p[1] != 0) {
    return TelemetryReadUtf16(guest_address, max_length);
  }
  return TelemetryReadAnsi(guest_address, max_length);
}

std::string TelemetryHexDump(uint32_t guest_address, uint32_t length) {
  if (!TelemetryGuestReadable(guest_address, length)) {
    return guest_address ? fmt::format("<unreadable {:08X}>", guest_address)
                         : std::string("<null>");
  }
  auto p = kernel_memory()->TranslateVirtual<const uint8_t*>(guest_address);
  std::string out;
  out.reserve(length * 2);
  for (uint32_t i = 0; i < length; ++i) {
    out += fmt::format("{:02X}", p[i]);
  }
  return out;
}

std::string TelemetryFormatGuid(uint32_t guest_address) {
  if (!TelemetryGuestReadable(guest_address, 16)) {
    return TelemetryHexDump(guest_address, 16);
  }
  auto p = kernel_memory()->TranslateVirtual<const uint8_t*>(guest_address);
  return fmt::format(
      "{{{:08X}-{:04X}-{:04X}-{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}"
      "}}",
      xe::load_and_swap<uint32_t>(p), xe::load_and_swap<uint16_t>(p + 4),
      xe::load_and_swap<uint16_t>(p + 6), p[8], p[9], p[10], p[11], p[12],
      p[13], p[14], p[15]);
}

uint64_t TelemetryGuestArg(const cpu::ppc::PPCContext* ctx, uint32_t n) {
  if (n < 8) {
    return ctx->r[3 + n];
  }
  // Each argument owns an 8-byte slot at caller r1 + 0x10 + 8 * n.
  uint32_t slot = uint32_t(ctx->r[1]) + 0x10 + 8 * n;
  if (!TelemetryGuestReadable(slot, 8)) {
    return 0;
  }
  return xe::load_and_swap<uint64_t>(
      kernel_memory()->TranslateVirtual<const uint8_t*>(slot));
}

}  // namespace kernel
}  // namespace xe

/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the project root for more  *
 * information.                                                               *
 ******************************************************************************
 */

// Guest-side telemetry entry points. None of these send anything anywhere:
// they record what the guest asked to report into the telemetry log
// (see xenia/kernel/telemetry_log.h) and fail or succeed the way the
// offline system would.

#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/telemetry_log.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {

namespace {

std::string Printable(uint32_t guest_address, uint32_t length) {
  if (!length) {
    return std::string();
  }
  if (!TelemetryGuestReadable(guest_address, length)) {
    return TelemetryHexDump(guest_address, length);
  }
  auto p = kernel_memory()->TranslateVirtual<const uint8_t*>(guest_address);
  bool text = std::all_of(p, p + length, [](uint8_t c) {
    return c == '\r' || c == '\n' || c == '\t' || (c >= 0x20 && c < 0x7F);
  });
  if (text) {
    std::string s(reinterpret_cast<const char*>(p), length);
    std::replace(s.begin(), s.end(), '\r', ' ');
    std::replace(s.begin(), s.end(), '\n', ' ');
    return s;
  }
  return "hex:" + TelemetryHexDump(guest_address, length);
}

// --- XamWebInstrumentation
// ----------------------------------------------------

struct WebReport {
  std::string name;
  uint32_t sample_rate = 100;
  std::vector<std::pair<std::string, std::string>> vars;
};
std::mutex web_report_mutex;
std::map<uint32_t, WebReport> web_reports;

}  // namespace

// XamWebInstrumentationCreateSampledReport(name, sample_rate, out_handle).
// The real XAM allocates a 0xE30-byte CWebReporting object and later sends it
// to http://<host>/msgserver/logstring2.ashx (or a URL from the Live hive).
dword_result_t XamWebInstrumentationCreateSampledReport_entry(
    lpu16string_t name, dword_t sample_rate, lpdword_t out_handle) {
  if (!out_handle) {
    return X_E_INVALIDARG;
  }
  *out_handle = 0;
  std::string report_name = name ? xe::to_utf8(name.value()) : std::string();
  if (report_name.size() > 0x10) {
    return X_E_INVALIDARG;
  }
  uint32_t handle = kernel_memory()->SystemHeapAlloc(0x10);
  if (!handle) {
    return X_E_FAIL;
  }
  {
    std::lock_guard<std::mutex> lock(web_report_mutex);
    auto& report = web_reports[handle];
    report.name = report_name;
    report.sample_rate = sample_rate;
  }
  TelemetryLog("WebInstrumentation",
               fmt::format("CreateReport handle={:08X} name=\"{}\" rate={}",
                           handle, report_name, uint32_t(sample_rate)));
  *out_handle = handle;
  return X_E_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamWebInstrumentationCreateSampledReport, kNone, kStub);

dword_result_t XamWebInstrumentationCreateReport_entry(lpu16string_t name,
                                                       lpdword_t out_handle) {
  return XamWebInstrumentationCreateSampledReport_entry(name, 100, out_handle);
}
DECLARE_XAM_EXPORT1(XamWebInstrumentationCreateReport, kNone, kStub);

namespace {
// SetUserVar(handle, var_index, value): XAM keeps up to 0x28 numbered
// variables per report (index 0x26 is a repeatable slot).
void SetWebReportVar(uint32_t handle, uint32_t var_index, uint32_t value_ptr,
                     bool escape) {
  if (handle == 0 || handle == 0xFFFFFFFF) {
    return;
  }
  if (var_index > 0x27) {
    return;
  }
  std::string name = fmt::format("v{}", var_index);
  std::string value = TelemetryReadAnyString(value_ptr, 1024);
  {
    std::lock_guard<std::mutex> lock(web_report_mutex);
    auto it = web_reports.find(handle);
    if (it == web_reports.end()) {
      return;
    }
    it->second.vars.emplace_back(name, value);
  }
  TelemetryLog("WebInstrumentation",
               fmt::format("SetUserVar{} handle={:08X} {}={}",
                           escape ? "" : "NoEscape", handle, name, value));
}
}  // namespace

void XamWebInstrumentationSetUserVar_entry(dword_t handle, dword_t name,
                                           dword_t value) {
  SetWebReportVar(handle, name, value, true);
}
DECLARE_XAM_EXPORT1(XamWebInstrumentationSetUserVar, kNone, kStub);

void XamWebInstrumentationSetUserVarNoEscape_entry(dword_t handle, dword_t name,
                                                   dword_t value) {
  SetWebReportVar(handle, name, value, false);
}
DECLARE_XAM_EXPORT1(XamWebInstrumentationSetUserVarNoEscape, kNone, kStub);

void XamWebInstrumentationSendReport_entry(dword_t handle) {
  if (handle == 0 || handle == 0xFFFFFFFF) {
    return;
  }
  std::string line;
  {
    std::lock_guard<std::mutex> lock(web_report_mutex);
    auto it = web_reports.find(handle);
    if (it == web_reports.end()) {
      return;
    }
    line = fmt::format("SendReport handle={:08X} name=\"{}\" query=\"",
                       uint32_t(handle), it->second.name);
    bool first = true;
    for (auto& [k, v] : it->second.vars) {
      line += fmt::format("{}{}={}", first ? "" : "&", k, v);
      first = false;
    }
    line += "\" (blocked, not sent)";
  }
  TelemetryLog("WebInstrumentation", line);
}
DECLARE_XAM_EXPORT1(XamWebInstrumentationSendReport, kNone, kStub);

void XamWebInstrumentationDestroyReport_entry(lpdword_t handle_ptr) {
  if (!handle_ptr) {
    return;
  }
  uint32_t handle = *handle_ptr;
  if (handle == 0 || handle == 0xFFFFFFFF) {
    return;
  }
  bool found;
  {
    std::lock_guard<std::mutex> lock(web_report_mutex);
    found = web_reports.erase(handle) != 0;
  }
  if (found) {
    kernel_memory()->SystemHeapFree(handle);
    TelemetryLog("WebInstrumentation",
                 fmt::format("DestroyReport handle={:08X}", handle));
  }
  *handle_ptr = 0;
}
DECLARE_XAM_EXPORT1(XamWebInstrumentationDestroyReport, kNone, kStub);

void XampWebInstrumentationSetProfileCounts_entry(dword_t live_profiles,
                                                  dword_t local_profiles,
                                                  dword_t adult_profiles,
                                                  dword_t child_profiles) {
  TelemetryLog("WebInstrumentation",
               fmt::format("SetProfileCounts live={} local={} adult={} "
                           "child={}",
                           uint32_t(live_profiles), uint32_t(local_profiles),
                           uint32_t(adult_profiles), uint32_t(child_profiles)));
}
DECLARE_XAM_EXPORT1(XampWebInstrumentationSetProfileCounts, kUserProfiles,
                    kStub);

// --- Data center / localization / Kinect reports
// ------------------------------

// XamDataCenterLog(category, level, format, ...): forwarded to XamDcaLog only
// when the data-center bit (0x8E03860A bit 2) is set.
void XamDataCenterLog_entry(const ppc_context_t& ctx) {
  uint32_t category = uint32_t(ctx->r[3]) & 0xFFFF;
  uint32_t level = uint32_t(ctx->r[4]) & 0xFFFF;
  std::string format = TelemetryReadAnyString(uint32_t(ctx->r[5]), 1024);
  std::string message = TelemetryFormatGuest(
      format, [&](uint32_t n) { return TelemetryGuestArg(ctx, 3 + n); });
  TelemetryLog("DataCenterLog", fmt::format("category={:04X} level={} \"{}\"",
                                            category, level, message));
}
DECLARE_XAM_EXPORT1(XamDataCenterLog, kNone, kStub);

dword_result_t XamIsDataCenterMode_entry() { return 0; }
DECLARE_XAM_EXPORT1(XamIsDataCenterMode, kNone, kStub);

// XamLogLocalizationEtx(event_code, data): 0x80300035 = missing glyph,
// 0x80300034 = resource fallback.
dword_result_t XamLogLocalizationEtx_entry(dword_t event_code, dword_t data) {
  const char* kind = event_code == 0x80300035   ? "MissingGlyph"
                     : event_code == 0x80300034 ? "ResourceFallback"
                                                : nullptr;
  if (!kind) {
    // Real XAM only handles the two codes above and returns E_NOTIMPL.
    TelemetryLog("LocalizationEtx",
                 fmt::format("event={:08X} data={:08X} (unhandled)",
                             uint32_t(event_code), uint32_t(data)));
    return X_E_NOT_IMPLEMENTED;
  }
  std::string detail = TelemetryReadAnyString(data, 256);
  TelemetryLog("LocalizationEtx",
               fmt::format("{} data={:08X}{}{}", kind, uint32_t(data),
                           detail.empty() ? "" : " ", detail));
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamLogLocalizationEtx, kNone, kStub);

dword_result_t XamReportKinectSettingsChangedEvent_entry(dword_t a1, dword_t a2,
                                                         dword_t a3,
                                                         dword_t a4) {
  TelemetryLog("KinectSettingsChanged",
               fmt::format("args={:08X} {:08X} {:08X} {:08X}", uint32_t(a1),
                           uint32_t(a2), uint32_t(a3), uint32_t(a4)));
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamReportKinectSettingsChangedEvent, kNone, kStub);

// --- XHttp request capture
// ----------------------------------------------------- With
// --telemetry_capture_xhttp the guest gets real (dummy) handles, builds its
// requests, and every send fails with ERROR_WINHTTP_CANNOT_CONNECT after the
// request is written to the telemetry log.

namespace {
constexpr uint32_t kErrorWinHttpCannotConnect = 12029;

enum class HttpHandleType { kSession, kConnect, kRequest };
struct HttpHandle {
  HttpHandleType type;
  uint32_t parent = 0;
  std::string user_agent;
  std::string host;
  uint32_t port = 0;
  uint32_t flags = 0;
  std::string verb;
  std::string path;
  uint32_t body_bytes = 0;
};
std::mutex http_mutex;
std::map<uint32_t, HttpHandle> http_handles;

uint32_t NewHttpHandle(HttpHandle info) {
  uint32_t handle = kernel_memory()->SystemHeapAlloc(0x10);
  if (handle) {
    std::lock_guard<std::mutex> lock(http_mutex);
    http_handles[handle] = std::move(info);
  }
  return handle;
}

bool GetHttpHandle(uint32_t handle, HttpHandle* out) {
  std::lock_guard<std::mutex> lock(http_mutex);
  auto it = http_handles.find(handle);
  if (it == http_handles.end()) {
    return false;
  }
  *out = it->second;
  return true;
}

std::string RequestUrl(const HttpHandle& request) {
  HttpHandle connect;
  if (!GetHttpHandle(request.parent, &connect)) {
    return request.path;
  }
  // XHTTP_FLAG_SECURE / WINHTTP_FLAG_SECURE = 0x00800000.
  bool secure = ((request.flags | connect.flags) & 0x00800000) != 0;
  return fmt::format("{}://{}:{}{}", secure ? "https" : "http", connect.host,
                     connect.port, request.path);
}
}  // namespace

dword_result_t NetDll_XHttpOpen_entry(dword_t caller, lpstring_t user_agent,
                                      dword_t access_type,
                                      lpstring_t proxy_name,
                                      lpstring_t proxy_bypass, dword_t flags) {
  if ((access_type != 0 && access_type != 1 && access_type != 3) ||
      proxy_bypass) {
    XThread::SetLastError(X_ERROR_INVALID_PARAMETER);
    return 0;
  }
  std::string agent = user_agent ? std::string(user_agent.value()) : "";
  if (!cvars::telemetry_capture_xhttp) {
    TelemetryLog("XHttp", fmt::format("Open agent=\"{}\" (refused, capture "
                                      "disabled)",
                                      agent));
    XThread::SetLastError(X_ERROR_NOT_SUPPORTED);
    return 0;
  }
  HttpHandle info{HttpHandleType::kSession};
  info.user_agent = agent;
  info.flags = flags;
  uint32_t handle = NewHttpHandle(info);
  TelemetryLog("XHttp",
               fmt::format("Open session={:08X} agent=\"{}\"", handle, agent));
  XThread::SetLastError(handle ? X_ERROR_SUCCESS : X_ERROR_NOT_ENOUGH_MEMORY);
  return handle;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpOpen, kNetworking, kStub);

dword_result_t NetDll_XHttpConnect_entry(dword_t caller, dword_t session,
                                         lpstring_t server, dword_t port,
                                         dword_t flags) {
  HttpHandle parent;
  if (!GetHttpHandle(session, &parent) ||
      parent.type != HttpHandleType::kSession || !server) {
    XThread::SetLastError(X_ERROR_INVALID_HANDLE);
    return 0;
  }
  HttpHandle info{HttpHandleType::kConnect};
  info.parent = session;
  info.host = std::string(server.value());
  info.port = port ? uint32_t(port) : ((flags & 0x00800000) ? 443 : 80);
  info.flags = flags;
  uint32_t handle = NewHttpHandle(info);
  TelemetryLog("XHttp", fmt::format("Connect handle={:08X} host={} port={}",
                                    handle, info.host, info.port));
  return handle;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpConnect, kNetworking, kStub);

dword_result_t NetDll_XHttpOpenRequest_entry(dword_t caller, dword_t connect,
                                             lpstring_t verb, lpstring_t path,
                                             lpstring_t version,
                                             lpstring_t referrer,
                                             dword_t accept_types,
                                             dword_t flags) {
  HttpHandle parent;
  if (!GetHttpHandle(connect, &parent) ||
      parent.type != HttpHandleType::kConnect) {
    XThread::SetLastError(X_ERROR_INVALID_HANDLE);
    return 0;
  }
  HttpHandle info{HttpHandleType::kRequest};
  info.parent = connect;
  info.verb = verb ? std::string(verb.value()) : "GET";
  info.path = path ? std::string(path.value()) : "/";
  info.flags = flags;
  uint32_t handle = NewHttpHandle(info);
  TelemetryLog("XHttp", fmt::format("OpenRequest handle={:08X} {} {}", handle,
                                    info.verb, RequestUrl(info)));
  return handle;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpOpenRequest, kNetworking, kStub);

dword_result_t NetDll_XHttpSendRequest_entry(
    dword_t caller, dword_t request, dword_t headers, dword_t headers_length,
    dword_t optional, dword_t optional_length, dword_t total_length,
    dword_t context) {
  HttpHandle info;
  if (!GetHttpHandle(request, &info) || info.type != HttpHandleType::kRequest) {
    XThread::SetLastError(X_ERROR_INVALID_HANDLE);
    return 0;
  }
  std::string header_text;
  if (headers) {
    header_text =
        headers_length == 0xFFFFFFFF
            ? TelemetryReadAnsi(headers, 4096)
            : Printable(headers, std::min<uint32_t>(headers_length, 4096));
    std::replace(header_text.begin(), header_text.end(), '\r', ' ');
    std::replace(header_text.begin(), header_text.end(), '\n', ' ');
  }
  TelemetryLog(
      "XHttp",
      fmt::format(
          "SendRequest {} {} headers=\"{}\" body({}/{})=\"{}\" "
          "(blocked, not sent)",
          info.verb, RequestUrl(info), header_text, uint32_t(optional_length),
          uint32_t(total_length),
          Printable(optional, std::min<uint32_t>(optional_length, 4096))));
  XThread::SetLastError(kErrorWinHttpCannotConnect);
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpSendRequest, kNetworking, kStub);

dword_result_t NetDll_XHttpWriteData_entry(dword_t caller, dword_t request,
                                           dword_t buffer, dword_t length,
                                           lpdword_t written) {
  HttpHandle info;
  if (GetHttpHandle(request, &info)) {
    TelemetryLog(
        "XHttp",
        fmt::format("WriteData {} {} ({} bytes)=\"{}\" "
                    "(blocked)",
                    info.verb, RequestUrl(info), uint32_t(length),
                    Printable(buffer, std::min<uint32_t>(length, 4096))));
  }
  if (written) {
    *written = 0;
  }
  XThread::SetLastError(kErrorWinHttpCannotConnect);
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpWriteData, kNetworking, kStub);

dword_result_t NetDll_XHttpReceiveResponse_entry(dword_t caller,
                                                 dword_t request,
                                                 dword_t reserved) {
  XThread::SetLastError(kErrorWinHttpCannotConnect);
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpReceiveResponse, kNetworking, kStub);

dword_result_t NetDll_XHttpQueryHeaders_entry(dword_t caller, dword_t request,
                                              dword_t info_level, dword_t name,
                                              dword_t buffer,
                                              lpdword_t buffer_length,
                                              lpdword_t index) {
  XThread::SetLastError(kErrorWinHttpCannotConnect);
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpQueryHeaders, kNetworking, kStub);

dword_result_t NetDll_XHttpReadData_entry(dword_t caller, dword_t request,
                                          dword_t buffer, dword_t length,
                                          lpdword_t read) {
  if (read) {
    *read = 0;
  }
  XThread::SetLastError(kErrorWinHttpCannotConnect);
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpReadData, kNetworking, kStub);

dword_result_t NetDll_XHttpSetStatusCallback_entry(dword_t caller,
                                                   dword_t handle,
                                                   dword_t callback,
                                                   dword_t notification_flags,
                                                   dword_t reserved) {
  return 0;  // previous callback
}
DECLARE_XAM_EXPORT1(NetDll_XHttpSetStatusCallback, kNetworking, kStub);

dword_result_t NetDll_XHttpSetOption_entry(dword_t caller, dword_t handle,
                                           dword_t option, dword_t buffer,
                                           dword_t length) {
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpSetOption, kNetworking, kStub);

dword_result_t NetDll_XHttpQueryOption_entry(dword_t caller, dword_t handle,
                                             dword_t option, dword_t buffer,
                                             lpdword_t length) {
  XThread::SetLastError(X_ERROR_INVALID_PARAMETER);
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpQueryOption, kNetworking, kStub);

dword_result_t NetDll_XHttpDoWork_entry(dword_t caller, dword_t session,
                                        dword_t timeout) {
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpDoWork, kNetworking, kStub);

dword_result_t NetDll_XHttpCloseHandle_entry(dword_t caller, dword_t handle) {
  bool found;
  {
    std::lock_guard<std::mutex> lock(http_mutex);
    found = http_handles.erase(handle) != 0;
  }
  if (!found) {
    XThread::SetLastError(X_ERROR_INVALID_HANDLE);
    return 0;
  }
  kernel_memory()->SystemHeapFree(handle);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpCloseHandle, kNetworking, kStub);

dword_result_t NetDll_XHttpShutdown_entry(dword_t caller) { return 1; }
DECLARE_XAM_EXPORT1(NetDll_XHttpShutdown, kNetworking, kStub);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(Telemetry);

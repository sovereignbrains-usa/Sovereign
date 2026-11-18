#include "pipe_access.h"

#include <windows.h>

namespace sovereign::service {

namespace {

// Windows paths compare without case; CompareStringOrdinal folds it the way
// the file system does (a user-mode string compare, no locale).
bool SamePath(const std::wstring& a, const std::wstring& b) {
  return !a.empty() && a.size() == b.size() &&
         CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) ==
             CSTR_EQUAL;
}

}  // namespace

bool IsAllowedPipeClient(const PipeClient& client, const std::optional<std::wstring>& trustedImage) {
  if (!trustedImage) {
    return true;
  }
  return client.elevated || SamePath(client.image, *trustedImage);
}

}  // namespace sovereign::service

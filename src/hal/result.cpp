/**
 * @file      result.cpp
 * @brief     hal_error_string() — see result.h.
 */
#include "result.h"

const char *hal_error_string(HalError e)
{
    switch (e) {
    case HalError::Ok:                  return "OK";
    case HalError::InvalidArgument:     return "Invalid argument.";
    case HalError::NotSupported:        return "Not supported on this device.";
    case HalError::InternalError:       return "Internal error.";

    case HalError::StorageFull:         return "Storage is full.";

    case HalError::WifiOffline:         return "WiFi is offline.";
    case HalError::HttpError:           return "Server returned an error.";
    case HalError::Unauthorized:        return "Unauthorized (check token).";

    case HalError::HubDisabled:         return "Local hub is disabled.";
    case HalError::HubUnreachable:      return "Local hub is unreachable.";
    }
    return "Unknown error.";
}

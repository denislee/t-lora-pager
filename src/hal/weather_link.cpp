/**
 * @file      weather_link.cpp
 * @brief     weather_link_get() — see weather_link.h.
 *
 * One forecast over PagerLink: weather.get with the cache key, decode the
 * reply with weather_payload_parse(). Blocking; the caller runs it on a worker
 * task/thread.
 */
#include "weather_link.h"

#include "link.h"
#include "link/link_json.h"

// Map a failed hw_link_call() to a HalError plus a UI-ready message.
static HalError link_fail(const HwLinkResult &r, std::string &err)
{
    if (r.code == "disconnected") {
        err = hal_error_string(HalError::LinkOffline);
        return HalError::LinkOffline;
    }
    err = "phone: " + r.code + ": " + r.msg;
    return HalError::InternalError;
}

HalResult<WeatherPayload> weather_link_get(const std::string &key, std::string &err)
{
    err.clear();

    if (key.empty()) {
        err = "no weather key";
        return HalError::InvalidArgument;
    }

    if (!hw_link_connected()) {
        err = hal_error_string(HalError::LinkOffline);
        return HalError::LinkOffline;
    }

    pagerlink::JsonWriter w;
    w.begin_object().key("key").str_val(key).end_object();

    const HwLinkResult r = hw_link_call("weather.get", w.str(), WEATHER_GET_TIMEOUT_MS);
    if (!r.ok) return link_fail(r, err);

    WeatherPayload p;
    std::string why;
    if (!weather_payload_parse(r.json, p, why)) {
        err = "phone: bad weather.get reply: " + why;
        return HalError::InternalError;
    }
    return p;
}

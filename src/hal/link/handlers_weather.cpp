/**
 * @file      handlers_weather.cpp
 * @brief     PagerLink weather.* handlers (PROTOCOL.md §6, IOS_COMPANION_PLAN.md Track C 9).
 *
 * weather.push (P->D): the phone hands the pager a ForecastPayload
 * (hal/weather_payload.h). A valid one is stored in the weather cache under
 * its key — the same store the Weather app reads — which bumps
 * weather_cache_generation() so an open Weather app re-renders. An invalid one
 * is refused whole with invalid_argument; nothing is clamped or half-stored.
 */
#include <string>

#include "../result.h"
#include "../weather_cache.h"
#include "../weather_payload.h"

#include "hal/link/link_json.h"
#include "hal/link/link_rpc.h"
#include "hal/link/link_service.h"

namespace pagerlink {
namespace {

bool handle_weather_push(const JsonObject &params, JsonWriter &result, RpcError &err) {
    WeatherPayload p;
    std::string why;
    if (!weather_payload_parse(params.text(), p, why)) {
        err.code = hal_error_code(HalError::InvalidArgument);
        err.msg = why;
        return false;
    }
    weather_cache_save_forecast(p.key, p.fc);
    if (p.has_geo) weather_cache_save_geo(p.key, p.geo);
    result.begin_object().end_object();
    return true;
}

}  // namespace

void register_weather_handlers(Rpc &rpc) {
    rpc.handle("weather.push", handle_weather_push);
}

}  // namespace pagerlink

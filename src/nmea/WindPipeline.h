#pragma once
// ============================================================================
// WindPipeline - the one place wind values enter the DataModel.
//
// Thin glue between the bus/demo and WindCalc::Pipeline (data.windPipe): it
// collects the boat inputs from the model, hands in the settings from the
// config - the four damping time constants (cal.wind_damp_app_spd /
// _app_dir / _true_spd / _true_dir, see config/WindDampCfg.h) and whether
// COG may stand in for a missing heading in TWD (cal.twd_cog) - read on
// every sample, so a WebUI save applies at once, and copies the five damped
// outputs back into data.aws/awa/tws/twa/twd. The rules themselves - when
// true wind can be computed, which heading counts as true, what stands in
// for it, when a received true wind is used, when values go stale - are in
// WindCalc.h.
//
// EVERY FUNCTION HERE MUST BE CALLED WITH data.lock() HELD. The mutex is not
// recursive, so they do not take it themselves.
// ============================================================================
#include <stdint.h>

namespace WindPipeline {

// One apparent wind sample, calibration already applied (PGN 130306 ref 2,
// or the demo). NaN for a field the sender marked "not available".
void apparent(float awsKn, float awaDeg, uint32_t nowMs);

// One true wind sample from another device (PGN 130306 ref 4, 3 or 0, in the
// bus's numbering): TWA for 3/4, TWD for 0. Only used as a fallback.
void receivedTrue(uint8_t ref, float twsKn, float angleDeg, uint32_t nowMs);

// The clock: blanks wind that has gone stale and takes the 1 Hz wind history
// sample. Called from the display tick, which runs in every mode - bus, demo
// and PC simulator alike.
void tick(uint32_t nowMs);

} // namespace WindPipeline

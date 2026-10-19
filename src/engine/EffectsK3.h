// Gridline — κ3 effects (Time Gate engine) and their extra entry points.
#pragma once
#include <memory>
#include <string>
#include "Effects.h"

namespace gl {

std::unique_ptr<Effect> createEffectK3(const std::string& type);
// Time Gate: a note selects a slot (time slots 60..95, volume slots 24..59; key-held slots return on release).
void timeGateNote(Effect* fx, int pitch, bool on);

}  // namespace gl

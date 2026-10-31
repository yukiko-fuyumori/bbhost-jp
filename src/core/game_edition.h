#pragma once

#include <string>
#include "bbhost_edition.h"

const char* game_edition_name();
// Check native base/update metadata before overlays; empty means accepted.
std::string game_edition_error(const std::string& base, const std::string& update);

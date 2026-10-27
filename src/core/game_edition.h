#pragma once

#include <string>
#include "bbhost_edition.h"

enum class GameEdition { JapaneseOldHunters, EuropeanGoty };
inline constexpr GameEdition kBuildGameEdition = BBHOST_PERSONAL_EU
    ? GameEdition::EuropeanGoty : GameEdition::JapaneseOldHunters;

const char* game_edition_name(GameEdition edition = kBuildGameEdition);
// Check native base/update metadata before overlays; empty means accepted.
std::string game_edition_error(const std::string& base, const std::string& update,
                              GameEdition edition = kBuildGameEdition);

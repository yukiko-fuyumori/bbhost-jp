#pragma once

#include <string>
#include "bbhost_edition.h"

enum class GameEdition { JapaneseOldHunters, EuropeanGoty };
inline constexpr GameEdition kBuildGameEdition = BBHOST_PERSONAL_EU
    ? GameEdition::EuropeanGoty : GameEdition::JapaneseOldHunters;

const char* game_edition_name(GameEdition edition = kBuildGameEdition);
// Read original files directly, before mods or plugins can supply an overlay.
// Both a base game and a separate update must belong to the selected edition.
// Empty means accepted; missing, malformed or mismatched metadata fails closed.
std::string game_edition_error(const std::string& base, const std::string& update,
                              GameEdition edition = kBuildGameEdition);

#pragma once
#include "raylib.h"
#include "game/spells.hpp"

namespace game::sfx {

    // Placeholders for future audio — call sites are wired; bodies are no-ops for now.
    inline void PlaySpellCast(SpellId id, Vector3 /*pos*/) {
        (void)id;
        // TODO: PlaySound(GetSpellDef(id).sfxCast);
    }

    inline void PlaySpellImpact(SpellId id, Vector3 /*pos*/) {
        (void)id;
        // TODO: PlaySound(GetSpellDef(id).sfxImpact);
    }

    inline void PlayMeleeSwing(int /*dir*/) {
        // TODO: whoosh on strike frame
    }

    inline void PlayMeleeImpact(Vector3 /*pos*/) {
        // TODO: blade hit / flesh
    }

} // namespace game::sfx

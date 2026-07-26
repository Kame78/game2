#pragma once
#include "raylib.h"
#include <cstdint>
#include <cstddef>

namespace game {

    enum class SpellId : uint8_t {
        Fireball   = 0,
        Fireblast  = 1,
        SuperNova  = 2,
        Firewall   = 3,
        Inferno    = 4,
        Ignite     = 5,
        Splash     = 6,
        Waterjet   = 7,
        Geyser     = 8,
        Waterwall  = 9,
        Whirlpool  = 10,
        Hurricane  = 11,
        Surge      = 12, // forward wave (formerly Tsunami)
        Maelstrom  = 13,
        // Necromancer
        SummonPixie    = 14, // steal 15 HP/s for 3s
        SummonGargoyle = 15, // combat pet
        CallOfTheDead  = 16, // undead AoE at aim
        // Priest
        SummonSprite      = 17, // heal 15 HP/s for 3s
        SummonBattleAngel = 18, // combat pet
        // Fire ultimates / eruptions
        LavaPlume    = 19,
        HellOnEarth  = 20,
        // Class ultimates (Maelstrom / Hell-on-Earth scale)
        SummonReaper     = 21, // Necro
        SummonArchAngel  = 22, // Priest
        // Ranger
        Dash       = 23, // quick 20 ft burst
        Teleport   = 24, // blink to aim point
        DoubleJump = 25, // passive mid-air jump
        Tsunami    = 26, // mega forward wave (~120 ft wide)
        Count
    };

    enum class SpellDelivery : uint8_t {
        Projectile,
        InstantAoE,
        PersistentZone,
        Hitscan,
        MovingWave,
        SelfBuff,   // applies timed effect to caster (pixie / sprite)
        SummonPet,  // spawns a temporary ally
        Mobility,   // dash / teleport
        Passive,    // always-on while class selected (not cast)
    };

    enum class SpellElement : uint8_t {
        Fire         = 0,
        Water        = 1,
        Necromancer  = 2,
        Priest       = 3,
        Ranger       = 4,
        Count
    };

    struct SpellDef {
        const char* name;
        SpellId id;
        SpellDelivery delivery;
        SpellElement element;
        float damage;            // hit damage, DPS for zones, or heal/drain rate for buffs
        float aoeRadius;
        float projectileSpeed;
        float projectileRadius;
        float lifetime;
        float manaCost;
        float cooldown;
        float castTime;
        float burnDps;
        float burnDuration;
        float force;
        float travelSpeed;
        float extra;             // pet lifetime, column height, etc.
        bool piercing;
        bool damageOnce;
        const char* sfxCast;
        const char* sfxImpact;
    };

    inline constexpr SpellDef SPELL_DEFS[] = {
        // --- Fire ---
        {
            "Fireball", SpellId::Fireball, SpellDelivery::Projectile, SpellElement::Fire,
            50.0f, 4.0f, 25.0f, 0.25f, 3.0f,
            10.0f, 0.40f, 0.0f,
            0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false, false,
            "sfx_fireball_cast", "sfx_fireball_impact"
        },
        {
            "Fireblast", SpellId::Fireblast, SpellDelivery::Projectile, SpellElement::Fire,
            70.0f, 5.5f, 28.0f, 0.40f, 3.0f,
            18.0f, 0.70f, 0.15f,
            0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false, false,
            "sfx_fireblast_cast", "sfx_fireblast_impact"
        },
        {
            "Super Nova", SpellId::SuperNova, SpellDelivery::InstantAoE, SpellElement::Fire,
            135.0f, 8.0f, 0.0f, 0.0f, 0.75f,
            35.0f, 2.00f, 0.40f,
            0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false, false,
            "sfx_supernova_cast", "sfx_supernova_impact"
        },
        {
            "Firewall", SpellId::Firewall, SpellDelivery::PersistentZone, SpellElement::Fire,
            30.0f, 5.0f, 0.0f, 0.0f, 8.0f,
            40.0f, 4.00f, 0.50f,
            20.0f, 2.0f, 0.0f, 0.0f, 0.0f, false, false,
            "sfx_firewall_cast", "sfx_firewall_loop"
        },
        {
            "Inferno", SpellId::Inferno, SpellDelivery::InstantAoE, SpellElement::Fire,
            500.0f, 14.0f, 0.0f, 0.0f, 1.20f,
            80.0f, 8.00f, 0.80f,
            0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false, false,
            "sfx_inferno_cast", "sfx_inferno_impact"
        },
        {
            "Ignite", SpellId::Ignite, SpellDelivery::Hitscan, SpellElement::Fire,
            650.0f, 1.2f, 120.0f, 0.15f, 0.35f,
            90.0f, 6.00f, 0.35f,
            0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false, false,
            "sfx_ignite_cast", "sfx_ignite_impact"
        },
        // --- Water ---
        {
            "Splash", SpellId::Splash, SpellDelivery::Projectile, SpellElement::Water,
            30.0f, 3.2f, 22.0f, 0.22f, 2.5f,
            8.0f, 0.35f, 0.0f,
            0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false, false,
            "sfx_splash_cast", "sfx_splash_impact"
        },
        {
            "Waterjet", SpellId::Waterjet, SpellDelivery::Projectile, SpellElement::Water,
            70.0f, 0.9f, 45.0f, 0.18f, 1.8f,
            16.0f, 0.60f, 0.10f,
            0.0f, 0.0f, 0.0f, 0.0f, 0.0f, true, false,
            "sfx_waterjet_cast", "sfx_waterjet_impact"
        },
        {
            "Geyser", SpellId::Geyser, SpellDelivery::InstantAoE, SpellElement::Water,
            120.0f, 4.5f, 0.0f, 0.0f, 1.1f,
            28.0f, 1.80f, 0.35f,
            0.0f, 0.0f, 0.0f, 0.0f, 9.0f, false, false,
            "sfx_geyser_cast", "sfx_geyser_impact"
        },
        {
            "Waterwall", SpellId::Waterwall, SpellDelivery::PersistentZone, SpellElement::Water,
            10.0f, 5.5f, 0.0f, 0.0f, 8.0f,
            35.0f, 4.00f, 0.45f,
            0.0f, 0.0f, 14.0f, 0.0f, 0.0f, false, false,
            "sfx_waterwall_cast", "sfx_waterwall_loop"
        },
        {
            "Whirlpool", SpellId::Whirlpool, SpellDelivery::PersistentZone, SpellElement::Water,
            10.0f, 7.0f, 0.0f, 0.0f, 7.0f,
            40.0f, 5.00f, 0.50f,
            0.0f, 0.0f, -9.0f, 0.0f, 0.0f, false, false,
            "sfx_whirlpool_cast", "sfx_whirlpool_loop"
        },
        {
            "Hurricane", SpellId::Hurricane, SpellDelivery::PersistentZone, SpellElement::Water,
            150.0f, 12.0f, 0.0f, 0.0f, 3.0f,
            70.0f, 10.00f, 0.70f,
            0.0f, 0.0f, -3.0f, 0.0f, 0.0f, false, false,
            "sfx_hurricane_cast", "sfx_hurricane_loop"
        },
        {
            "Surge", SpellId::Surge, SpellDelivery::MovingWave, SpellElement::Water,
            750.0f, 5.0f, 0.0f, 0.0f, 4.0f,
            95.0f, 12.00f, 1.00f,
            0.0f, 0.0f, 22.0f, 16.0f, 12.0f, false, true,
            "sfx_surge_cast", "sfx_surge_impact"
        },
        {
            "Maelstrom", SpellId::Maelstrom, SpellDelivery::PersistentZone, SpellElement::Water,
            350.0f, 120.0f, 0.0f, 0.0f, 8.0f,
            130.0f, 22.00f, 1.20f,
            0.0f, 0.0f, -22.0f, 0.0f, 0.0f, false, false,
            "sfx_maelstrom_cast", "sfx_maelstrom_loop"
        },
        // --- Necromancer ---
        {
            "Summon Pixie", SpellId::SummonPixie, SpellDelivery::SelfBuff, SpellElement::Necromancer,
            15.0f, 10.0f, 0.0f, 0.0f, 3.0f, // damage = drain HP/s, aoe = drain range, lifetime = duration
            25.0f, 8.00f, 0.25f,
            0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false, false,
            "sfx_pixie_cast", "sfx_pixie_loop"
        },
        {
            "Summon Gargoyle", SpellId::SummonGargoyle, SpellDelivery::SummonPet, SpellElement::Necromancer,
            28.0f, 2.5f, 0.0f, 0.0f, 14.0f, // damage = pet melee, lifetime = pet duration
            45.0f, 14.00f, 0.50f,
            0.0f, 0.0f, 0.0f, 0.0f, 14.0f, false, false,
            "sfx_gargoyle_cast", "sfx_gargoyle_loop"
        },
        {
            "Call of the Dead", SpellId::CallOfTheDead, SpellDelivery::PersistentZone, SpellElement::Necromancer,
            60.0f, 7.0f, 0.0f, 0.0f, 4.0f, // DPS in targeted area
            50.0f, 10.00f, 0.55f,
            0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false, false,
            "sfx_calldead_cast", "sfx_calldead_loop"
        },
        // --- Priest ---
        {
            "Summon Sprite", SpellId::SummonSprite, SpellDelivery::SelfBuff, SpellElement::Priest,
            15.0f, 0.0f, 0.0f, 0.0f, 3.0f, // damage field = heal/s
            25.0f, 8.00f, 0.25f,
            0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false, false,
            "sfx_sprite_cast", "sfx_sprite_loop"
        },
        {
            "Summon Battle Angel", SpellId::SummonBattleAngel, SpellDelivery::SummonPet, SpellElement::Priest,
            35.0f, 3.0f, 0.0f, 0.0f, 14.0f,
            50.0f, 14.00f, 0.55f,
            0.0f, 0.0f, 0.0f, 0.0f, 14.0f, false, false,
            "sfx_angel_cast", "sfx_angel_loop"
        },
        {
            // Tall lava eruption — burst damage + lingering burning column
            "Lava Plume", SpellId::LavaPlume, SpellDelivery::InstantAoE, SpellElement::Fire,
            200.0f, 9.0f, 0.0f, 0.0f, 3.0f,
            42.0f, 5.00f, 0.40f,
            28.0f, 2.5f, 0.0f, 0.0f, 16.0f, false, false, // extra = plume height
            "sfx_lavaplume_cast", "sfx_lavaplume_impact"
        },
        {
            // Combined Lava Plume + Inferno burst + Firewall ring, scaled up.
            "Hell on Earth", SpellId::HellOnEarth, SpellDelivery::InstantAoE, SpellElement::Fire,
            720.0f, 120.0f, 0.0f, 0.0f, 10.0f,
            120.0f, 20.00f, 1.00f,
            40.0f, 3.5f, 0.0f, 0.0f, 24.0f, false, false, // extra = mega plume height
            "sfx_hellonearth_cast", "sfx_hellonearth_impact"
        },
        {
            // Maelstrom-scale necro ultimate — giant reaper, 120-unit death aura
            "Summon Reaper", SpellId::SummonReaper, SpellDelivery::SummonPet, SpellElement::Necromancer,
            180.0f, 120.0f, 0.0f, 0.0f, 20.0f,
            130.0f, 22.00f, 1.20f,
            0.0f, 0.0f, 0.0f, 0.0f, 20.0f, false, false,
            "sfx_reaper_cast", "sfx_reaper_loop"
        },
        {
            // Maelstrom-scale priest ultimate — giant archangel, 120-unit holy aura
            "Summon Arch Angel", SpellId::SummonArchAngel, SpellDelivery::SummonPet, SpellElement::Priest,
            160.0f, 120.0f, 0.0f, 0.0f, 20.0f,
            130.0f, 22.00f, 1.20f,
            0.0f, 0.0f, 0.0f, 0.0f, 20.0f, false, false,
            "sfx_archangel_cast", "sfx_archangel_loop"
        },
        // --- Ranger ---
        {
            // Burst 20 ft (6.096 m) along look / move direction
            "Dash", SpellId::Dash, SpellDelivery::Mobility, SpellElement::Ranger,
            0.0f, 0.0f, 0.0f, 0.0f, 0.14f, // lifetime = dash duration
            12.0f, 2.50f, 0.0f,
            0.0f, 0.0f, 0.0f, 0.0f, 6.096f, false, false, // extra = distance (meters)
            "sfx_dash_cast", "sfx_dash_cast"
        },
        {
            // Blink to targeted ground point (aoeRadius = max range)
            "Teleport", SpellId::Teleport, SpellDelivery::Mobility, SpellElement::Ranger,
            0.0f, 35.0f, 0.0f, 0.0f, 0.0f,
            28.0f, 8.00f, 0.12f,
            0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false, false,
            "sfx_teleport_cast", "sfx_teleport_impact"
        },
        {
            // Passive: one mid-air jump while Ranger is selected
            "Double Jump", SpellId::DoubleJump, SpellDelivery::Passive, SpellElement::Ranger,
            0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 0.00f, 0.0f,
            0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false, false,
            "sfx_doublejump", "sfx_doublejump"
        },
        {
            // Mega wall of water — ~120 ft (36.6 m) across
            "Tsunami", SpellId::Tsunami, SpellDelivery::MovingWave, SpellElement::Water,
            1400.0f, 14.0f, 0.0f, 0.0f, 7.0f, // depth half-extent, lifetime
            140.0f, 24.00f, 1.40f,
            // force, travelSpeed, extra = half-width (60 ft each side → 120 ft total)
            0.0f, 0.0f, 32.0f, 14.0f, 18.288f, false, true,
            "sfx_tsunami_cast", "sfx_tsunami_impact"
        },
    };

    static_assert(sizeof(SPELL_DEFS) / sizeof(SPELL_DEFS[0]) == (std::size_t)SpellId::Count);
    static_assert((std::size_t)SpellId::Count <= 32);

    inline const SpellDef& GetSpellDef(SpellId id) {
        return SPELL_DEFS[(std::size_t)id];
    }

    inline const SpellDef& GetSpellDef(int id) {
        if (id < 0 || id >= (int)SpellId::Count) return SPELL_DEFS[0];
        return SPELL_DEFS[id];
    }

    inline int GetSpellsForElement(SpellElement el, SpellId* out, int maxOut) {
        int n = 0;
        for (int i = 0; i < (int)SpellId::Count && n < maxOut; i++) {
            if (SPELL_DEFS[i].element == el) out[n++] = (SpellId)i;
        }
        return n;
    }

    inline int CountSpellsForElement(SpellElement el) {
        int n = 0;
        for (int i = 0; i < (int)SpellId::Count; i++) {
            if (SPELL_DEFS[i].element == el) n++;
        }
        return n;
    }

    inline const char* ElementName(SpellElement el) {
        switch (el) {
            case SpellElement::Fire:        return "FIRE";
            case SpellElement::Water:       return "WATER";
            case SpellElement::Necromancer: return "NECRO";
            case SpellElement::Priest:      return "PRIEST";
            case SpellElement::Ranger:      return "RANGER";
            default: return "?";
        }
    }

    inline Color ElementColor(SpellElement el) {
        switch (el) {
            case SpellElement::Fire:        return Color{255, 120, 40, 255};
            case SpellElement::Water:       return Color{80, 170, 255, 255};
            case SpellElement::Necromancer: return Color{160, 60, 200, 255};
            case SpellElement::Priest:      return Color{255, 230, 120, 255};
            case SpellElement::Ranger:      return Color{90, 200, 90, 255};
            default: return WHITE;
        }
    }

    inline SpellElement NextElement(SpellElement el) {
        int i = ((int)el + 1) % (int)SpellElement::Count;
        return (SpellElement)i;
    }

} // namespace game

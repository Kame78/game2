#include "game/dungeon/dungeon.hpp"
#include "engine/data/json.hpp"

namespace game::dungeon {

namespace {

    GenProfile               g_profile;
    CampaignDef              g_campaign;
    std::vector<ModifierDef> g_modifiers;
    bool                     g_loaded = false;

    void installDefaults() {
        g_profile = GenProfile{};

        g_campaign.id   = "crypt_proto";
        g_campaign.name = "Sunken Crypt";
        g_campaign.stages.clear();
        g_campaign.stages.push_back(StageDef{"Upper Halls", 1.0f, 1.0f, 1, 0});
        g_campaign.stages.push_back(StageDef{"Flooded Ossuary", 1.25f, 1.2f, 2, 1});
        g_campaign.stages.push_back(StageDef{"Black Sanctum", 1.5f, 1.45f, 3, 1});

        g_modifiers.clear();
        g_modifiers.push_back(ModifierDef{"vigor", "Reinforced Dead (+40% enemy health)",
                                         12, 1.4f, 1.0f, 1.0f, 0.0f, 1.0f, 1.25f});
        g_modifiers.push_back(ModifierDef{"savage", "Savage (+35% enemy damage)",
                                         12, 1.0f, 1.35f, 1.0f, 0.0f, 1.0f, 1.25f});
        g_modifiers.push_back(ModifierDef{"swarm", "Swarming (+50% pack size)",
                                         10, 1.0f, 1.0f, 1.0f, 0.0f, 1.5f, 1.35f});
        g_modifiers.push_back(ModifierDef{"champions", "Champion Packs (elites in combat rooms)",
                                         8, 1.0f, 1.0f, 1.0f, 0.45f, 1.0f, 1.5f});
        g_modifiers.push_back(ModifierDef{"frenzy", "Frenzied (enemies move faster)",
                                         10, 1.0f, 1.0f, 1.45f, 0.0f, 1.0f, 1.3f});
        g_modifiers.push_back(ModifierDef{"hoard", "Hoarded Wealth (+80% reward)",
                                         6, 1.15f, 1.0f, 1.0f, 0.0f, 1.0f, 1.8f});
    }

}  // namespace

void LoadData() {
    if (g_loaded) return;
    g_loaded = true;
    installDefaults();

    const engine::data::Json profile =
        engine::data::LoadJsonFile("assets/data/dungeons/generation_profile.json");
    if (profile.IsObject()) {
        g_profile.minPathRooms = profile.Int("minPathRooms", g_profile.minPathRooms);
        g_profile.maxPathRooms = profile.Int("maxPathRooms", g_profile.maxPathRooms);
        g_profile.maxBranches  = profile.Int("maxBranches",  g_profile.maxBranches);
        g_profile.cell         = profile.Float("cell",         g_profile.cell);
        g_profile.secretChance = profile.Float("secretChance", g_profile.secretChance);
        g_profile.vaultChance  = profile.Float("vaultChance",  g_profile.vaultChance);
        g_profile.maskCellSize = profile.Float("maskCellSize", g_profile.maskCellSize);
        g_profile.largeCombatChance = profile.Float("largeCombatChance", g_profile.largeCombatChance);
        g_profile.smallSpanMin  = profile.Float("smallSpanMin",  g_profile.smallSpanMin);
        g_profile.smallSpanMax  = profile.Float("smallSpanMax",  g_profile.smallSpanMax);
        g_profile.mediumSpanMin = profile.Float("mediumSpanMin", g_profile.mediumSpanMin);
        g_profile.mediumSpanMax = profile.Float("mediumSpanMax", g_profile.mediumSpanMax);
        g_profile.largeSpanMin  = profile.Float("largeSpanMin",  g_profile.largeSpanMin);
        g_profile.largeSpanMax  = profile.Float("largeSpanMax",  g_profile.largeSpanMax);
        g_profile.bossSpanMin   = profile.Float("bossSpanMin",   g_profile.bossSpanMin);
        g_profile.bossSpanMax   = profile.Float("bossSpanMax",   g_profile.bossSpanMax);
    }

    const engine::data::Json mods =
        engine::data::LoadJsonFile("assets/data/dungeons/modifiers.json");
    const engine::data::Json& modList = mods.IsArray() ? mods : mods.Get("modifiers");
    if (modList.IsArray() && modList.Size() > 0) {
        g_modifiers.clear();
        for (size_t i = 0; i < modList.Size(); ++i) {
            const engine::data::Json& m = modList.At(i);
            if (!m.IsObject()) continue;
            ModifierDef def;
            def.id               = m.Str("id", "mod");
            def.name             = m.Str("name", def.id.c_str());
            def.weight           = m.Int("weight", 10);
            def.monsterHealthMul = m.Float("monsterHealthMul", 1.0f);
            def.monsterDamageMul = m.Float("monsterDamageMul", 1.0f);
            def.monsterSpeedMul  = m.Float("monsterSpeedMul", 1.0f);
            def.eliteChance      = m.Float("eliteChance", 0.0f);
            def.extraEnemyMul    = m.Float("extraEnemyMul", 1.0f);
            def.rewardMul        = m.Float("rewardMul", 1.0f);
            g_modifiers.push_back(std::move(def));
        }
    }

    const engine::data::Json campaign =
        engine::data::LoadJsonFile("assets/data/dungeons/campaign_proto.json");
    if (campaign.IsObject()) {
        g_campaign.id   = campaign.Str("id", g_campaign.id.c_str());
        g_campaign.name = campaign.Str("name", g_campaign.name.c_str());

        const engine::data::Json& stages = campaign.Get("stages");
        if (stages.IsArray() && stages.Size() > 0) {
            g_campaign.stages.clear();
            for (size_t i = 0; i < stages.Size(); ++i) {
                const engine::data::Json& s = stages.At(i);
                if (!s.IsObject()) continue;
                StageDef def;
                def.name          = s.Str("name", "Stage");
                def.enemyCountMul = s.Float("enemyCountMul", 1.0f);
                def.healthMul     = s.Float("healthMul", 1.0f);
                def.modifierRolls = s.Int("modifierRolls", 1);
                def.extraBranches = s.Int("extraBranches", 0);
                g_campaign.stages.push_back(std::move(def));
            }
        }
    }

    if (g_campaign.stages.empty()) {
        g_campaign.stages.push_back(StageDef{"Crypt", 1.0f, 1.0f, 1, 0});
    }
    if (g_modifiers.empty()) {
        g_modifiers.push_back(ModifierDef{"plain", "Unmodified", 10, 1.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f});
    }
}

const GenProfile& GetGenProfile() {
    LoadData();
    return g_profile;
}

const CampaignDef& GetCampaign() {
    LoadData();
    return g_campaign;
}

const std::vector<ModifierDef>& GetModifierPool() {
    LoadData();
    return g_modifiers;
}

}  // namespace game::dungeon

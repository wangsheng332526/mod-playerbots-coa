/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CoaEnchanter.h"

#include "CharacterCache.h"
#include "CoaAltAddon.h"
#include "CoaMissionMgr.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "ItemUsageValue.h"
#include "Mail.h"
#include "ObjectMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotMgr.h"
#include "PlayerbotOperation.h"
#include "PlayerbotWorldThreadProcessor.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace
{
constexpr char const* TEAM_KEY = "coa_enchanter";
constexpr time_t SKILL_CACHE_SECONDS = 10 * MINUTE;

std::mutex skillLock;
std::unordered_map<ObjectGuid::LowType, std::pair<uint32, time_t>> skillCache;

constexpr time_t GEAR_REFRESH_SECONDS = MINUTE;

// What each alt of a team logged in can wear, read in the world thread (the alts live on other maps than the bot
// asking): its class and race, level, weapon skills, the heaviest armour it is trained for, and the item level of
// what it wears in each slot (0: nothing there).
struct AltGear
{
    ObjectGuid::LowType guid = 0;
    uint32 classMask = 0;
    uint32 raceMask = 0;
    uint32 level = 0;
    uint32 weapons = 0;
    uint32 armor = ITEM_SUBCLASS_ARMOR_CLOTH;
    std::array<uint32, EQUIPMENT_SLOT_END> worn = {};
};

// The equipment slots an item of this inventory type goes in (rings, trinkets, necks, cloaks and shirts: none).
std::vector<uint8> SlotsOf(uint32 inventoryType)
{
    switch (inventoryType)
    {
        case INVTYPE_HEAD: return {EQUIPMENT_SLOT_HEAD};
        case INVTYPE_SHOULDERS: return {EQUIPMENT_SLOT_SHOULDERS};
        case INVTYPE_CHEST:
        case INVTYPE_ROBE: return {EQUIPMENT_SLOT_CHEST};
        case INVTYPE_WAIST: return {EQUIPMENT_SLOT_WAIST};
        case INVTYPE_LEGS: return {EQUIPMENT_SLOT_LEGS};
        case INVTYPE_FEET: return {EQUIPMENT_SLOT_FEET};
        case INVTYPE_WRISTS: return {EQUIPMENT_SLOT_WRISTS};
        case INVTYPE_HANDS: return {EQUIPMENT_SLOT_HANDS};
        case INVTYPE_WEAPON: return {EQUIPMENT_SLOT_MAINHAND, EQUIPMENT_SLOT_OFFHAND};
        case INVTYPE_WEAPONMAINHAND:
        case INVTYPE_2HWEAPON: return {EQUIPMENT_SLOT_MAINHAND};
        case INVTYPE_WEAPONOFFHAND:
        case INVTYPE_SHIELD:
        case INVTYPE_HOLDABLE: return {EQUIPMENT_SLOT_OFFHAND};
        case INVTYPE_RANGED:
        case INVTYPE_RANGEDRIGHT:
        case INVTYPE_THROWN:
        case INVTYPE_RELIC: return {EQUIPMENT_SLOT_RANGED};
        default: return {};
    }
}
struct TeamGear
{
    time_t at = 0;
    bool pending = false;
    std::vector<AltGear> alts;
};
std::mutex gearLock;
std::unordered_map<ObjectGuid::LowType, TeamGear> teamGear;

class CoaTeamGearOperation : public PlayerbotOperation
{
public:
    explicit CoaTeamGearOperation(ObjectGuid owner) : m_owner(owner) {}

    std::string GetName() const override { return "CoaTeamGear"; }

    bool Execute() override
    {
        std::vector<AltGear> alts;
        Player* master = ObjectAccessor::FindPlayer(m_owner);
        if (PlayerbotMgr* mgr = master ? GET_PLAYERBOT_MGR(master) : nullptr)
            for (auto itr = mgr->GetPlayerBotsBegin(); itr != mgr->GetPlayerBotsEnd(); ++itr)
            {
                Player* alt = itr->second;
                PlayerbotAI* altAI = alt ? GET_PLAYERBOT_AI(alt) : nullptr;
                if (!alt || !altAI || !altAI->IsAltBot())
                    continue;
                AltGear gear;
                gear.guid = alt->GetGUID().GetCounter();
                gear.classMask = alt->getClassMask();
                gear.raceMask = alt->getRaceMask();
                gear.level = alt->GetLevel();
                for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
                    if (Item* worn = alt->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                        gear.worn[slot] = worn->GetTemplate()->ItemLevel;
                gear.weapons = alt->GetWeaponProficiency();
                uint32 const armor = alt->GetArmorProficiency();
                for (uint32 kind : {ITEM_SUBCLASS_ARMOR_PLATE, ITEM_SUBCLASS_ARMOR_MAIL, ITEM_SUBCLASS_ARMOR_LEATHER})
                    if (armor & (1 << kind))
                    {
                        gear.armor = kind;
                        break;
                    }
                alts.push_back(gear);
            }
        std::lock_guard<std::mutex> guard(gearLock);
        TeamGear& team = teamGear[m_owner.GetCounter()];
        team.alts.swap(alts);
        team.at = time(nullptr);
        team.pending = false;
        return true;
    }

private:
    ObjectGuid m_owner;
};

// An upgrade for an alt of the team logged in, other than the bot: a weapon it can use or armour of the kind it
// wears, of its level or below, with a higher item level than what it wears in that slot (an empty slot too).
// Rings, necks, trinkets and cloaks count for nobody. From the cache, asked again in the world thread every minute.
bool TeamWouldWear(Player* bot, Player* master, ItemTemplate const* proto)
{
    if (!bot || !master)
        return false;
    bool const armor = proto->Class == ITEM_CLASS_ARMOR;
    if (armor && (proto->SubClass < ITEM_SUBCLASS_ARMOR_CLOTH || proto->SubClass > ITEM_SUBCLASS_ARMOR_PLATE ||
                  proto->InventoryType == INVTYPE_CLOAK))
        return false;

    ObjectGuid const owner = master->GetGUID();
    ObjectGuid::LowType const self = bot->GetGUID().GetCounter();
    std::vector<AltGear> alts;
    bool refresh = false;
    bool known = false;
    {
        std::lock_guard<std::mutex> guard(gearLock);
        TeamGear& team = teamGear[owner.GetCounter()];
        if (!team.pending && time(nullptr) - team.at >= GEAR_REFRESH_SECONDS)
            refresh = team.pending = true;
        known = team.at != 0;
        alts = team.alts;
    }
    if (refresh && !PlayerbotWorldThreadProcessor::instance().QueueOperation(std::make_unique<CoaTeamGearOperation>(owner)))
    {
        std::lock_guard<std::mutex> guard(gearLock);
        teamGear[owner.GetCounter()].pending = false;
    }
    // Not read yet: kept, as if someone would wear it.
    if (!known)
        return true;

    for (AltGear const& alt : alts)
    {
        if (alt.guid == self)
            continue;
        // As Player::CanUseItem: its class and race, its weapon skill.
        if (!(proto->AllowableClass & alt.classMask) || !(proto->AllowableRace & alt.raceMask))
            continue;
        if (!(armor ? proto->SubClass == alt.armor : (alt.weapons & (1 << proto->SubClass)) != 0))
            continue;
        if (proto->RequiredLevel > alt.level)
            continue;
        for (uint8 slot : SlotsOf(proto->InventoryType))
            if (proto->ItemLevel > alt.worn[slot])
                return true;
    }
    return false;
}

// Green or blue gear (never epic), disenchantable at maxSkill, of no use to the bot itself.
bool IsJunkTemplate(PlayerbotAI* botAI, ItemTemplate const* proto, uint32 maxSkill)
{
    if (!proto || proto->Quality < ITEM_QUALITY_UNCOMMON || proto->Quality > ITEM_QUALITY_RARE)
        return false;
    if (proto->Class != ITEM_CLASS_ARMOR && proto->Class != ITEM_CLASS_WEAPON)
        return false;
    if (!proto->DisenchantID || int32(proto->RequiredDisenchantSkill) < 0 ||
        (maxSkill && proto->RequiredDisenchantSkill > maxSkill))
        return false;
    if (proto->StartQuest || proto->Bonding == BIND_QUEST_ITEM || proto->Bonding == BIND_QUEST_ITEM1)
        return false;
    // The professions' tools stay.
    if (proto->Class == ITEM_CLASS_WEAPON && (proto->SubClass == ITEM_SUBCLASS_WEAPON_MISC ||
                                              proto->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE ||
                                              proto->TotemCategory != 0))
        return false;
    ItemUsage const usage = botAI->GetAiObjectContext()->GetValue<ItemUsage>("item usage", proto->ItemId)->Get();
    return usage == ITEM_USAGE_NONE || usage == ITEM_USAGE_VENDOR || usage == ITEM_USAGE_AH ||
           usage == ITEM_USAGE_BAD_EQUIP;
}
}  // namespace

namespace CoaEnchanter
{
// The player's limits for one item: a green from the alts or out of the bank, a blue bind on equip one; never more.
bool WithinLimits(Team const& team, ItemTemplate const* proto, bool bank)
{
    if (!proto)
        return false;
    if (proto->Quality == ITEM_QUALITY_RARE)
        return (team.limits & LIMIT_BLUES) && proto->Bonding == BIND_WHEN_EQUIPPED;
    if (proto->Quality == ITEM_QUALITY_UNCOMMON)
        return (team.limits & (bank ? LIMIT_BANK_GREENS : LIMIT_ALT_GREENS)) != 0;
    return false;
}

Team Of(ObjectGuid::LowType owner)
{
    Team team;
    if (!owner)
        return team;
    team.enchanter = sRandomPlayerbotMgr.GetValue(owner, TEAM_KEY);
    std::string const data = sRandomPlayerbotMgr.GetData(owner, TEAM_KEY);
    size_t const split = data.find(';');
    team.mode = data.compare(0, split, "1") == 0 ? MODE_BANK : MODE_MAIL;
    // "<mode>;<name>;<limits>" (no limits: the defaults).
    size_t const second = split == std::string::npos ? std::string::npos : data.find(';', split + 1);
    team.name = split == std::string::npos ? "" : data.substr(split + 1, second == std::string::npos ? std::string::npos
                                                                                                     : second - split - 1);
    if (second != std::string::npos)
        team.limits = uint32(std::strtoul(data.c_str() + second + 1, nullptr, 10)) & 0xF;
    // A mode chosen before any enchanter is stored with no name (value 0 would delete the row).
    if (team.name.empty())
        team.enchanter = 0;
    return team;
}

void Set(ObjectGuid::LowType owner, Team const& team)
{
    // The value is the enchanter's guid; with none named, 1 and no name (0 would delete the mode and the limits).
    uint32 const value = team.enchanter ? team.enchanter : 1;
    sRandomPlayerbotMgr.SetPermanentValue(owner, TEAM_KEY, value,
                                          std::to_string(team.mode) + ";" + (team.enchanter ? team.name : "") + ";" +
                                              std::to_string(team.limits));
}

Team TeamOf(PlayerbotAI* botAI)
{
    if (!sPlayerbotAIConfig.altDisenchant || !botAI || !botAI->IsAltBot() || !botAI->GetMaster())
        return Team();
    Team team = Of(botAI->GetMaster()->GetGUID().GetCounter());
    // The bank mode works on the configured tab only: none, by mail. Nothing allowed out of the bank: by mail too.
    if (team.mode == MODE_BANK && !sPlayerbotAIConfig.guildBankDepositTab)
    {
        team.mode = MODE_MAIL;
        team.bankWithoutTab = true;
    }
    if (team.mode == MODE_BANK && !(team.limits & (LIMIT_BANK_GREENS | LIMIT_BLUES)))
        team.mode = MODE_MAIL;
    return team;
}

bool IsEnchanter(PlayerbotAI* botAI)
{
    Team const team = TeamOf(botAI);
    return team.enchanter && team.enchanter == botAI->GetBot()->GetGUID().GetCounter();
}

uint32 EnchantingOf(ObjectGuid::LowType guid)
{
    if (!guid)
        return 0;
    if (Player* online = ObjectAccessor::FindPlayerByLowGUID(guid))
        return online->HasSkill(SKILL_ENCHANTING) ? online->GetSkillValue(SKILL_ENCHANTING) : 0;

    time_t const now = time(nullptr);
    {
        std::lock_guard<std::mutex> guard(skillLock);
        auto const it = skillCache.find(guid);
        if (it != skillCache.end() && now - it->second.second < SKILL_CACHE_SECONDS)
            return it->second.first;
    }
    uint32 skill = 0;
    if (QueryResult result = CharacterDatabase.Query("SELECT value FROM character_skills WHERE guid = {} AND skill = {}",
                                                     guid, uint32(SKILL_ENCHANTING)))
        skill = result->Fetch()[0].Get<uint32>();
    std::lock_guard<std::mutex> guard(skillLock);
    skillCache[guid] = {skill, now};
    return skill;
}

bool IsEnchantingMaterial(ItemTemplate const* proto)
{
    return proto && proto->Class == ITEM_CLASS_TRADE_GOODS && proto->SubClass == ITEM_SUBCLASS_ENCHANTING;
}

bool IsJunkToDisenchant(PlayerbotAI* botAI, Item* item, uint32 maxSkill, bool bank)
{
    if (!botAI || !item || item->IsEquipped() || item->IsInTrade())
        return false;
    ItemTemplate const* proto = item->GetTemplate();
    Team const team = TeamOf(botAI);
    if (!WithinLimits(team, proto, bank) || !IsJunkTemplate(botAI, proto, maxSkill))
        return false;
    // Bind on equip, still unbound: kept for an alt of the team it is an upgrade for, unless the player allows it.
    if (!(team.limits & LIMIT_WEARABLE) && proto->Bonding == BIND_WHEN_EQUIPPED && !item->IsSoulBound() &&
        TeamWouldWear(botAI->GetBot(), botAI->GetMaster(), proto))
        return false;
    return true;
}

bool IsJunkTemplateForEnchanter(PlayerbotAI* botAI, ItemTemplate const* proto, uint32 maxSkill)
{
    Team const team = TeamOf(botAI);
    return WithinLimits(team, proto, true) && IsJunkTemplate(botAI, proto, maxSkill) &&
           ((team.limits & LIMIT_WEARABLE) ||
            !(proto->Bonding == BIND_WHEN_EQUIPPED && TeamWouldWear(botAI->GetBot(), botAI->GetMaster(), proto)));
}

int32 DepositRule(PlayerbotAI* botAI, Item const* item)
{
    Team const team = TeamOf(botAI);
    if (!team.enchanter || !item)
        return -1;
    Item* bagItem = const_cast<Item*>(item);
    if (team.enchanter == botAI->GetBot()->GetGUID().GetCounter())
    {
        // The enchanter puts its dusts, essences and shards in the guild bank, and keeps the gear to disenchant.
        if (IsEnchantingMaterial(item->GetTemplate()))
            return 1;
        return IsJunkToDisenchant(botAI, bagItem, 0) ? 0 : -1;
    }
    // By the bank, it has to be allowed out of the bank too.
    if (item->IsSoulBound() || !IsJunkToDisenchant(botAI, bagItem, EnchantingOf(team.enchanter)) ||
        (team.mode == MODE_BANK && !IsJunkToDisenchant(botAI, bagItem, EnchantingOf(team.enchanter), true)))
        return -1;
    // By the guild bank: in it, for the enchanter. By mail: kept for the mail.
    return team.mode == MODE_BANK ? 1 : 0;
}

bool KeepFromVendor(PlayerbotAI* botAI, Item* item)
{
    Team const team = TeamOf(botAI);
    if (!team.enchanter || !item || item->GetTemplate()->Quality < ITEM_QUALITY_UNCOMMON)
        return false;
    bool const self = team.enchanter == botAI->GetBot()->GetGUID().GetCounter();
    if (!self && item->IsSoulBound())
        return false;
    // An upgrade for an alt of the team stays in the bags, neither sold nor disenchanted.
    ItemTemplate const* proto = item->GetTemplate();
    if (!(team.limits & LIMIT_WEARABLE) && proto->Bonding == BIND_WHEN_EQUIPPED && !item->IsSoulBound() &&
        (proto->Class == ITEM_CLASS_ARMOR || proto->Class == ITEM_CLASS_WEAPON) &&
        TeamWouldWear(botAI->GetBot(), botAI->GetMaster(), proto))
        return true;
    return IsJunkToDisenchant(botAI, item, self ? 0 : EnchantingOf(team.enchanter));
}

uint32 MailItems(Player* bot, ObjectGuid::LowType to, std::vector<Item*> const& items, std::string const& subject,
                 std::string const& body)
{
    // By entry: the mission's mailer takes the stacks of the bags (never soulbound, never equipped).
    std::map<uint32, uint32> counts;
    for (Item* item : items)
        if (item && !item->IsEquipped() && !item->IsSoulBound())
            counts[item->GetEntry()] += item->GetCount();
    return CoaMissionMgr::MailItems(bot, to, counts, subject, body);
}

void SendStatus(PlayerbotAI* botAI)
{
    if (!botAI || !botAI->IsAltBot() || !botAI->GetMaster() || !CoaAltAddon::AddonSeen(botAI))
        return;
    Player* bot = botAI->GetBot();
    Team const team = sPlayerbotAIConfig.altDisenchant ? Of(botAI->GetMaster()->GetGUID().GetCounter()) : Team();
    std::ostringstream out;
    out << "ENCH\t" << (team.name.empty() ? "-" : CoaAltAddon::Field(team.name, 24)) << "\t"
        << (team.mode == MODE_BANK ? "bank" : "mail") << "\t"
        << (bot->HasSkill(SKILL_ENCHANTING) ? bot->GetSkillValue(SKILL_ENCHANTING) : 0) << "\t"
        << (sPlayerbotAIConfig.altDisenchant ? 1 : 0) << "\t" << team.limits;
    CoaAltAddon::SendToMaster(botAI, out.str());
}

std::string Command(PlayerbotAI* botAI, Player* master, std::string const& arg)
{
    if (!sPlayerbotAIConfig.altDisenchant)
        return "The enchanter of the team is disabled on this server.";

    ObjectGuid::LowType const owner = master->GetGUID().GetCounter();
    Team team = Of(owner);
    std::istringstream in(arg);
    std::string word;
    in >> word;
    std::transform(word.begin(), word.end(), word.begin(), [](unsigned char c) { return std::tolower(c); });

    if (word == "set")
    {
        // The rest of the line: CoA names take two words ("Dez Alt"), and quotes are never part of a name.
        std::string name;
        std::getline(in, name);
        name.erase(std::remove_if(name.begin(), name.end(), [](char c) { return c == '"' || c == '\''; }), name.end());
        name.erase(0, name.find_first_not_of(' '));
        name.erase(name.find_last_not_of(' ') + 1);
        if (name.empty())
            return "Usage: alt enchanter set <name>";
        normalizePlayerName(name);
        ObjectGuid const guid = sCharacterCache->GetCharacterGuidByName(name);
        // One of the player's own characters: of the same account, or an alt the player has logged in now.
        PlayerbotMgr* mgr = GET_PLAYERBOT_MGR(master);
        bool const own = guid && (sCharacterCache->GetCharacterAccountIdByGuid(guid) == master->GetSession()->GetAccountId() ||
                                  (mgr && mgr->GetPlayerBot(guid)));
        if (!own)
            return "Enchanter refused: " + name + " is none of your characters.";
        if (!EnchantingOf(guid.GetCounter()))
            return "Enchanter refused: " + name + " does not know Enchanting.";
        team.enchanter = guid.GetCounter();
        team.name = name;
    }
    else if (word == "clear")
    {
        team.enchanter = 0;
        team.name.clear();
    }
    else if (word == "mode")
    {
        std::string mode;
        in >> mode;
        if (mode != "mail" && mode != "bank")
            return "Usage: alt enchanter mode mail|bank";
        team.mode = mode == "bank" ? MODE_BANK : MODE_MAIL;
    }
    else if (word == "limit")
    {
        std::string key;
        std::string state;
        in >> key >> state;
        uint32 const bit = key == "altgreens"    ? LIMIT_ALT_GREENS
                           : key == "bankgreens" ? LIMIT_BANK_GREENS
                           : key == "blues"      ? LIMIT_BLUES
                           : key == "wearable"   ? LIMIT_WEARABLE
                                                 : 0;
        if (!bit || (state != "on" && state != "off"))
            return "Usage: alt enchanter limit altgreens|bankgreens|blues|wearable on|off";
        team.limits = state == "on" ? (team.limits | bit) : (team.limits & ~bit);
    }
    else if (!word.empty() && word != "status")
        return "Usage: alt enchanter [set <name>|clear|mode mail|mode bank|limit <key> on|off]";

    if (!word.empty() && word != "status")
        Set(owner, team);

    // Every alt logged in tells the addon.
    if (PlayerbotMgr* mgr = GET_PLAYERBOT_MGR(master))
        for (auto itr = mgr->GetPlayerBotsBegin(); itr != mgr->GetPlayerBotsEnd(); ++itr)
            if (PlayerbotAI* altAI = itr->second ? GET_PLAYERBOT_AI(itr->second) : nullptr)
                SendStatus(altAI);

    std::string how = team.mode == MODE_BANK ? "through the guild bank" : "by mail";
    if (team.mode == MODE_BANK && !sPlayerbotAIConfig.guildBankDepositTab)
        how += " (not before the server sets AiPlayerbot.GuildBankDeposit.Tab: by mail meanwhile)";
    else if (team.mode == MODE_BANK && !(team.limits & (LIMIT_BANK_GREENS | LIMIT_BLUES)))
        how += " (nothing allowed out of the bank in the limits: by mail meanwhile)";
    else if (team.mode == MODE_BANK)
        how += " (tab " + std::to_string(sPlayerbotAIConfig.guildBankDepositTab) + ")";
    if (!team.enchanter)
        return "No enchanter in the team: the greens are sold (mode " + how + " kept for later).";
    return "Enchanter of the team: " + team.name + ", the greens go to it " + how + ".";
}
}  // namespace CoaEnchanter

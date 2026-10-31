/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CoaEnchanter.h"

#include "CharacterCache.h"
#include "CoaAltAddon.h"
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
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace
{
constexpr char const* TEAM_KEY = "coa_enchanter";
constexpr time_t SKILL_CACHE_SECONDS = 10 * MINUTE;

std::mutex skillLock;
std::unordered_map<ObjectGuid::LowType, std::pair<uint32, time_t>> skillCache;

// The heaviest armour an alt is trained for: what it would wear of the four kinds.
uint32 BestArmorOf(Player* alt)
{
    if (alt->HasSkill(SKILL_PLATE_MAIL))
        return ITEM_SUBCLASS_ARMOR_PLATE;
    if (alt->HasSkill(SKILL_MAIL))
        return ITEM_SUBCLASS_ARMOR_MAIL;
    if (alt->HasSkill(SKILL_LEATHER))
        return ITEM_SUBCLASS_ARMOR_LEATHER;
    return ITEM_SUBCLASS_ARMOR_CLOTH;
}

// An alt of the team logged in, other than the bot, would wear it: a weapon it can use, or armour of the kind it
// wears (a level too low still counts). Rings, necks, trinkets and cloaks count for nobody.
bool TeamWouldWear(Player* bot, Player* master, ItemTemplate const* proto)
{
    PlayerbotMgr* mgr = master ? GET_PLAYERBOT_MGR(master) : nullptr;
    if (!mgr)
        return false;
    bool const armor = proto->Class == ITEM_CLASS_ARMOR;
    if (armor && (proto->SubClass < ITEM_SUBCLASS_ARMOR_CLOTH || proto->SubClass > ITEM_SUBCLASS_ARMOR_PLATE ||
                  proto->InventoryType == INVTYPE_CLOAK))
        return false;
    for (auto itr = mgr->GetPlayerBotsBegin(); itr != mgr->GetPlayerBotsEnd(); ++itr)
    {
        Player* alt = itr->second;
        PlayerbotAI* altAI = alt ? GET_PLAYERBOT_AI(alt) : nullptr;
        if (!alt || alt == bot || !altAI || !altAI->IsAltBot())
            continue;
        InventoryResult const result = alt->CanUseItem(proto);
        if (result != EQUIP_ERR_OK && result != EQUIP_ERR_CANT_EQUIP_LEVEL_I)
            continue;
        if (!armor || proto->SubClass == BestArmorOf(alt))
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
Team Of(ObjectGuid::LowType owner)
{
    Team team;
    if (!owner)
        return team;
    team.enchanter = sRandomPlayerbotMgr.GetValue(owner, TEAM_KEY);
    std::string const data = sRandomPlayerbotMgr.GetData(owner, TEAM_KEY);
    size_t const split = data.find(';');
    team.mode = data.compare(0, split, "1") == 0 ? MODE_BANK : MODE_MAIL;
    team.name = split == std::string::npos ? "" : data.substr(split + 1);
    // A mode chosen before any enchanter is stored with no name (value 0 would delete the row).
    if (team.name.empty())
        team.enchanter = 0;
    return team;
}

void Set(ObjectGuid::LowType owner, Team const& team)
{
    // The value is the enchanter's guid; a player who chose a mode before naming one stores 1 with no name.
    uint32 const value = team.enchanter ? team.enchanter : (team.mode == MODE_BANK ? 1 : 0);
    sRandomPlayerbotMgr.SetPermanentValue(owner, TEAM_KEY, value,
                                          std::to_string(team.mode) + ";" + (team.enchanter ? team.name : ""));
}

Team TeamOf(PlayerbotAI* botAI)
{
    if (!sPlayerbotAIConfig.altDisenchant || !botAI || !botAI->IsAltBot() || !botAI->GetMaster())
        return Team();
    Team team = Of(botAI->GetMaster()->GetGUID().GetCounter());
    // The bank mode works on the configured tab only: none, by mail.
    if (team.mode == MODE_BANK && !sPlayerbotAIConfig.guildBankDepositTab)
    {
        team.mode = MODE_MAIL;
        team.bankWithoutTab = true;
    }
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

bool IsJunkToDisenchant(PlayerbotAI* botAI, Item* item, uint32 maxSkill)
{
    if (!botAI || !item || item->IsEquipped() || item->IsInTrade())
        return false;
    ItemTemplate const* proto = item->GetTemplate();
    if (!IsJunkTemplate(botAI, proto, maxSkill))
        return false;
    // Bind on equip, still unbound: kept for an alt of the team who would wear it.
    if (proto->Bonding == BIND_WHEN_EQUIPPED && !item->IsSoulBound() &&
        TeamWouldWear(botAI->GetBot(), botAI->GetMaster(), proto))
        return false;
    return true;
}

bool IsJunkTemplateForEnchanter(PlayerbotAI* botAI, ItemTemplate const* proto, uint32 maxSkill)
{
    return IsJunkTemplate(botAI, proto, maxSkill) &&
           !(proto->Bonding == BIND_WHEN_EQUIPPED && TeamWouldWear(botAI->GetBot(), botAI->GetMaster(), proto));
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
    if (item->IsSoulBound() || !IsJunkToDisenchant(botAI, bagItem, EnchantingOf(team.enchanter)))
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
    return IsJunkToDisenchant(botAI, item, self ? 0 : EnchantingOf(team.enchanter));
}

uint32 MailItems(Player* bot, ObjectGuid::LowType to, std::vector<Item*> const& items, std::string const& subject,
                 std::string const& body)
{
    if (!bot || !to || items.empty() || to == bot->GetGUID().GetCounter())
        return 0;

    ObjectGuid const receiverGuid = ObjectGuid::Create<HighGuid::Player>(to);
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    std::vector<Item*> toMail;
    uint32 count = 0;
    for (Item* item : items)
    {
        if (!item || item->IsEquipped() || item->IsSoulBound() || item->IsNotEmptyBag())
            continue;
        count += item->GetCount();
        bot->MoveItemFromInventory(item->GetBagSlot(), item->GetSlot(), true);
        item->DeleteFromInventoryDB(trans);
        item->SetOwnerGUID(receiverGuid);
        item->SaveToDB(trans);
        toMail.push_back(item);
    }
    if (toMail.empty())
    {
        CharacterDatabase.CommitTransaction(trans);
        return 0;
    }

    Player* receiver = ObjectAccessor::FindConnectedPlayer(receiverGuid);
    for (size_t first = 0; first < toMail.size(); first += MAX_MAIL_ITEMS)
    {
        MailDraft draft(subject, body);
        for (size_t i = first; i < toMail.size() && i < first + MAX_MAIL_ITEMS; ++i)
            draft.AddItem(toMail[i]);
        draft.SendMailTo(trans, MailReceiver(receiver, to), MailSender(bot));
    }
    bot->SaveInventoryAndGoldToDB(trans);
    CharacterDatabase.CommitTransaction(trans);
    return count;
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
        << (sPlayerbotAIConfig.altDisenchant ? 1 : 0);
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
        std::string name;
        in >> name;
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
    else if (!word.empty() && word != "status")
        return "Usage: alt enchanter [set <name>|clear|mode mail|mode bank]";

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
    else if (team.mode == MODE_BANK)
        how += " (tab " + std::to_string(sPlayerbotAIConfig.guildBankDepositTab) + ")";
    if (!team.enchanter)
        return "No enchanter in the team: the greens are sold (mode " + how + " kept for later).";
    return "Enchanter of the team: " + team.name + ", the greens go to it " + how + ".";
}
}  // namespace CoaEnchanter

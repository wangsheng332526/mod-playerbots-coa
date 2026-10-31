/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CoaEnchanterAction.h"

#include "Bag.h"
#include "CoaEnchanter.h"
#include "CoaGuildBankDepositAction.h"
#include "CoaMissionMgr.h"
#include "DatabaseEnv.h"
#include "Event.h"
#include "GameObject.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Item.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotOperation.h"
#include "PlayerbotWorldThreadProcessor.h"
#include "Playerbots.h"

#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <vector>

namespace
{
constexpr time_t CHECK_SECONDS = 5;
constexpr time_t IDLE_SECONDS = 30;
constexpr time_t MAIL_SECONDS = 10 * MINUTE;
constexpr time_t WITHDRAW_SECONDS = MINUTE;
constexpr time_t RIGHTS_TOLD_SECONDS = 30 * MINUTE;
constexpr uint32 MAX_WITHDRAW = 10;

// The players told that the bank mode needs a configured tab: once per player and server run.
std::mutex toldLock;
std::set<ObjectGuid::LowType> toldNoTab;

std::vector<Item*> BagItems(Player* bot)
{
    std::vector<Item*> items;
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            items.push_back(item);
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = bot->GetBagByPos(bagSlot))
            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                if (Item* item = bag->GetItemByPos(uint8(slot)))
                    items.push_back(item);
    return items;
}

bool FreeSlot(Player* bot, uint8& bagOut, uint8& slotOut)
{
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
        {
            bagOut = INVENTORY_SLOT_BAG_0;
            slotOut = slot;
            return true;
        }
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = bot->GetBagByPos(bagSlot))
        {
            // Only plain bags: a herb or ore bag takes no gear.
            ItemTemplate const* proto = bag->GetTemplate();
            if (proto && proto->SubClass != ITEM_SUBCLASS_CONTAINER)
                continue;
            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                if (!bag->GetItemByPos(uint8(slot)))
                {
                    bagOut = bagSlot;
                    slotOut = uint8(slot);
                    return true;
                }
        }
    return false;
}

// A tab of the guild bank where the bot's rank may put items (the configured tab, else any).
bool CanDepositSomewhere(Player* bot, Guild* guild)
{
    uint32 const wanted = sPlayerbotAIConfig.guildBankDepositTab;
    for (uint8 tab = 0; tab < GUILD_BANK_MAX_TABS; ++tab)
        if ((!wanted || tab == wanted - 1) && guild->MemberHasTabRights(bot->GetGUID(), tab, GUILD_BANK_RIGHT_DEPOSIT_ITEM))
            return true;
    return false;
}

// Mode bank: the enchanter near the vault takes out the gear it may disenchant, from the configured tab only (the
// tab the alts put it in), never anything else of the guild. Runs in the world thread, like the deposits: the guild
// bank is not thread-safe. The tab's content is read from the database, every item is checked again once in the
// bags (one that is not the expected one goes back).
class CoaEnchanterWithdrawOperation : public PlayerbotOperation
{
public:
    CoaEnchanterWithdrawOperation(ObjectGuid botGuid, ObjectGuid bankGuid) : m_botGuid(botGuid), m_bankGuid(bankGuid) {}

    ObjectGuid GetBotGuid() const override { return m_botGuid; }
    std::string GetName() const override { return "CoaEnchanterWithdraw"; }

    bool Execute() override
    {
        Player* bot = ObjectAccessor::FindPlayer(m_botGuid);
        PlayerbotAI* botAI = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
        if (!botAI || !bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat() || !CoaEnchanter::IsEnchanter(botAI))
            return false;
        Guild* guild = sGuildMgr->GetGuildById(bot->GetGuildId());
        GameObject* bank = bot->GetMap() ? bot->GetMap()->GetGameObject(m_bankGuid) : nullptr;
        if (!guild || !bank || !bot->IsWithinDistInMap(bank, 12.0f))
            return false;

        uint32 const skill = bot->HasSkill(SKILL_ENCHANTING) ? bot->GetSkillValue(SKILL_ENCHANTING) : 0;
        uint32 const configured = sPlayerbotAIConfig.guildBankDepositTab;
        if (!skill || !configured || configured > GUILD_BANK_MAX_TABS)
            return false;
        uint8 const tab = uint8(configured - 1);
        if (!guild->MemberHasTabRights(bot->GetGUID(), tab, GUILD_BANK_RIGHT_VIEW_TAB))
        {
            std::string const line = "Guild bank: my rank cannot see tab " + std::to_string(configured) + ".";
            LOG_INFO("playerbots", "Enchanter {} (guild {}): {}", bot->GetName(), guild->GetName(), line);
            botAI->TellMasterNoFacing(line);
            return false;
        }
        QueryResult result = CharacterDatabase.Query(
            "SELECT gbi.SlotId, ii.itemEntry FROM guild_bank_item gbi JOIN item_instance ii ON ii.guid = "
            "gbi.item_guid WHERE gbi.guildid = {} AND gbi.TabId = {} ORDER BY gbi.SlotId",
            guild->GetId(), uint32(tab));
        if (!result)
            return false;

        uint32 taken = 0;
        bool refused = false;
        do
        {
            if (taken >= MAX_WITHDRAW || bot->GetFreeInventorySpace() < 2)
                break;
            Field* fields = result->Fetch();
            uint8 const slot = fields[0].Get<uint8>();
            uint32 const entry = fields[1].Get<uint32>();
            if (!CoaEnchanter::IsJunkTemplateForEnchanter(botAI, sObjectMgr->GetItemTemplate(entry), skill))
                continue;
            uint8 bag = 0;
            uint8 bagSlot = 0;
            if (!FreeSlot(bot, bag, bagSlot))
                break;
            guild->SwapItemsWithInventory(bot, true, tab, slot, bag, bagSlot, 0);
            Item* got = bot->GetItemByPos(bag, bagSlot);
            if (!got)
            {
                // Nothing came: no withdraw right on the tab, or the rank's daily withdrawals are spent.
                refused = true;
                break;
            }
            if (got->GetEntry() != entry)
            {
                guild->SwapItemsWithInventory(bot, false, tab, slot, bag, bagSlot, 0);
                continue;
            }
            ++taken;
        } while (result->NextRow());

        std::ostringstream out;
        if (taken)
            out << "Guild bank: took out " << taken << (taken == 1 ? " item" : " items") << " to disenchant.";
        if (refused)
            out << (taken ? " " : "") << "Guild bank: my rank may not take items out of tab " << configured
                << " (withdraw right or daily limit).";
        if (refused)
            LOG_INFO("playerbots", "Enchanter {} (guild {}): {}", bot->GetName(), guild->GetName(), out.str());
        if (!out.str().empty())
            botAI->TellMasterNoFacing(out.str());
        return taken > 0;
    }

private:
    ObjectGuid m_botGuid;
    ObjectGuid m_bankGuid;
};
}  // namespace

bool CoaEnchanterAction::isUseful()
{
    return sPlayerbotAIConfig.altDisenchant && botAI->IsAltBot() && time(nullptr) >= nextCheck;
}

bool CoaEnchanterAction::Disenchant(uint32 skill)
{
    if (!bot->HasSpell(CoaEnchanter::DISENCHANT_SPELL) || bot->GetFreeInventorySpace() < 2 || bot->isMoving() ||
        bot->IsNonMeleeSpellCast(false) || bot->GetLootGUID() || bot->IsMounted())
        return false;
    for (Item* item : BagItems(bot))
        if (CoaEnchanter::IsJunkToDisenchant(botAI, item, skill))
        {
            LOG_DEBUG("playerbots", "Enchanter {} disenchants {}", bot->GetName(), item->GetEntry());
            return botAI->CastSpell(CoaEnchanter::DISENCHANT_SPELL, bot, item);
        }
    return false;
}

bool CoaEnchanterAction::Execute(Event /*event*/)
{
    time_t const now = time(nullptr);
    nextCheck = now + CHECK_SECONDS;

    if (!bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight() || bot->InBattleground() || bot->InArena())
        return false;
    CoaEnchanter::Team const team = CoaEnchanter::TeamOf(botAI);
    Player* master = botAI->GetMaster();
    if (!team.enchanter || !master)
    {
        nextCheck = now + IDLE_SECONDS;
        return false;
    }
    bool const onMission = sCoaMissionMgr.IsOnMission(bot);
    bool const self = team.enchanter == bot->GetGUID().GetCounter();

    // The bank mode with no tab configured works by mail: the player is told once.
    if (team.bankWithoutTab)
    {
        bool tell = false;
        {
            std::lock_guard<std::mutex> guard(toldLock);
            tell = toldNoTab.insert(master->GetGUID().GetCounter()).second;
        }
        if (tell)
        {
            LOG_INFO("playerbots", "Enchanter team of {}: bank mode without AiPlayerbot.GuildBankDeposit.Tab, by mail",
                     master->GetName());
            botAI->TellMasterNoFacing("Enchanter of the team: the guild bank mode needs a tab set on the server "
                                      "(AiPlayerbot.GuildBankDeposit.Tab); the greens go by mail meanwhile.");
        }
    }

    // Near a vault of a real player's guild: the deposits are the guild bank deposit action's; a rank without the
    // rights is told once in a while.
    GameObject* vault = nullptr;
    Guild* guild = bot->GetGuildId() ? sGuildMgr->GetGuildById(bot->GetGuildId()) : nullptr;
    if (guild && CoaGuildBankDepositAction::IsRealPlayerGuild(guild) && !onMission)
        vault = CoaGuildBankDepositAction::FindGuildBank(bot);
    if (vault && team.mode == CoaEnchanter::MODE_BANK && now - rightsToldAt >= RIGHTS_TOLD_SECONDS)
    {
        std::string missing;
        if (!sPlayerbotAIConfig.guildBankDepositEnable)
            missing = "the guild bank deposits are off on this server (AiPlayerbot.GuildBankDeposit.Enable)";
        else if (!CanDepositSomewhere(bot, guild))
            missing = "my rank may not put items in the guild bank";
        if (!missing.empty())
        {
            rightsToldAt = now;
            LOG_INFO("playerbots", "Enchanter team of {}: {} ({})", master->GetName(), missing, bot->GetName());
            botAI->TellMasterNoFacing("Enchanter of the team: " + missing + ".");
        }
    }

    if (self)
    {
        uint32 const skill = bot->HasSkill(SKILL_ENCHANTING) ? bot->GetSkillValue(SKILL_ENCHANTING) : 0;
        if (!skill)
            return false;
        if (!onMission && Disenchant(skill))
            return true;

        if (vault && team.mode == CoaEnchanter::MODE_BANK && now >= nextWithdraw && bot->GetFreeInventorySpace() >= 2)
        {
            nextWithdraw = now + WITHDRAW_SECONDS;
            auto operation = std::make_unique<CoaEnchanterWithdrawOperation>(bot->GetGUID(), vault->GetGUID());
            return PlayerbotWorldThreadProcessor::instance().QueueOperation(std::move(operation));
        }

        // Mode mail, away from a vault: the materials go to the master.
        if (team.mode == CoaEnchanter::MODE_MAIL && !vault && now >= nextMail)
        {
            nextMail = now + MAIL_SECONDS;
            std::vector<Item*> materials;
            for (Item* item : BagItems(bot))
                if (CoaEnchanter::IsEnchantingMaterial(item->GetTemplate()) && !item->IsSoulBound())
                    materials.push_back(item);
            if (uint32 const sent = CoaEnchanter::MailItems(bot, master->GetGUID().GetCounter(), materials,
                                                             "Enchanting materials", "What I got from disenchanting."))
            {
                botAI->TellMasterNoFacing("Enchanter of the team: " + std::to_string(sent) +
                                          " enchanting materials sent to you by mail.");
                return true;
            }
        }
        return false;
    }

    // Mode mail: the greens go to the enchanter, at most every 10 minutes.
    if (team.mode != CoaEnchanter::MODE_MAIL || now < nextMail)
        return false;
    nextMail = now + MAIL_SECONDS;
    uint32 const skill = CoaEnchanter::EnchantingOf(team.enchanter);
    if (!skill)
        return false;
    std::vector<Item*> greens;
    for (Item* item : BagItems(bot))
        if (!item->IsSoulBound() && CoaEnchanter::IsJunkToDisenchant(botAI, item, skill))
            greens.push_back(item);
    uint32 const sent = CoaEnchanter::MailItems(bot, team.enchanter, greens, "To disenchant",
                                                 "Gear nobody of the team wears, to disenchant.");
    if (sent)
        LOG_DEBUG("playerbots", "{} mailed {} items to the enchanter {}", bot->GetName(), sent, team.name);
    return sent > 0;
}

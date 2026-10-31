/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COAENCHANTER_H
#define PLAYERBOTS_COAENCHANTER_H

#include "Common.h"
#include "ObjectGuid.h"

#include <string>
#include <vector>

class Item;
class Player;
class PlayerbotAI;
struct ItemTemplate;

// The enchanter of the team (2.8.4, AiPlayerbot.AltDisenchant): a player names one of their alts that knows
// Enchanting ("alt enchanter set <name>", from the addon). It disenchants the green or better gear nobody of the
// team wears; the other alts send it theirs instead of selling them, by mail or through the guild bank (the mode,
// chosen by the player). Without an enchanter named, nothing changes: the greens are sold as before.
namespace CoaEnchanter
{
constexpr uint32 MODE_MAIL = 0;
constexpr uint32 MODE_BANK = 1;
constexpr uint32 DISENCHANT_SPELL = 13262;

// What the player lets the enchanter disenchant ("alt enchanter limit <key> on|off"). Epics: never.
constexpr uint32 LIMIT_ALT_GREENS = 0x1;   // altgreens: the greens of the alts' bags (its own too)
constexpr uint32 LIMIT_BANK_GREENS = 0x2;  // bankgreens: greens out of the guild bank
constexpr uint32 LIMIT_BLUES = 0x4;        // blues: blue bind on equip gear, from the alts and the bank
constexpr uint32 LIMIT_WEARABLE = 0x8;     // wearable: even gear an alt of the team could wear
constexpr uint32 LIMITS_DEFAULT = LIMIT_ALT_GREENS;

struct Team
{
    ObjectGuid::LowType enchanter = 0;  // 0: none named
    uint32 mode = MODE_MAIL;
    std::string name;
    uint32 limits = LIMITS_DEFAULT;
    // TeamOf: the bank mode was chosen, but no guild bank tab is configured (AiPlayerbot.GuildBankDeposit.Tab):
    // the greens go by mail meanwhile.
    bool bankWithoutTab = false;
};

// The choice of a player, kept in playerbots_random_bots (key coa_enchanter on the player's guid, no expiry).
Team Of(ObjectGuid::LowType owner);
void Set(ObjectGuid::LowType owner, Team const& team);

// The team of an alt: its master's choice, the mode as it works (by mail while no tab is configured, or while the
// limits let nothing out of the bank); empty when AltDisenchant is off or the bot is no alt.
Team TeamOf(PlayerbotAI* botAI);
bool IsEnchanter(PlayerbotAI* botAI);
// Enchanting of a character, logged in or not.
uint32 EnchantingOf(ObjectGuid::LowType guid);

// Gear of its bags that goes to the enchanter: within the player's limits (greens, blue bind on equip, never epic),
// disenchantable (at most maxSkill, 0 = any), no use to the bot itself (as "s junk" judges it), and unless the player
// allows it none an alt of the team logged in could wear. bank: on its way through the guild bank.
bool IsJunkToDisenchant(PlayerbotAI* botAI, Item* item, uint32 maxSkill = 0, bool bank = false);
bool IsEnchantingMaterial(ItemTemplate const* proto);
// The same for an item of the guild bank, known by its template (bind on equip, never soulbound there).
bool IsJunkTemplateForEnchanter(PlayerbotAI* botAI, ItemTemplate const* proto, uint32 maxSkill);

// The rule of the enchanter for the guild bank deposit: 1 deposit, 0 keep, -1 no opinion (the configured classes).
int32 DepositRule(PlayerbotAI* botAI, Item const* item);
// "s junk": a green the enchanter of the team will take, not sold.
bool KeepFromVendor(PlayerbotAI* botAI, Item* item);

// Mails items of the bags to a character (logged in or not); whole stacks. Returns how many items.
uint32 MailItems(Player* bot, ObjectGuid::LowType to, std::vector<Item*> const& items, std::string const& subject,
                 std::string const& body);

// "ENCH\t<enchanter|->\t<mail|bank>\t<this alt's Enchanting>\t<AltDisenchant 0|1>" to the master's addon.
void SendStatus(PlayerbotAI* botAI);
// "alt enchanter [set <name>|clear|mode mail|mode bank]": the answer in plain words.
std::string Command(PlayerbotAI* botAI, Player* master, std::string const& arg);
}  // namespace CoaEnchanter

#endif

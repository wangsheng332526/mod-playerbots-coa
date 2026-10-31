/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COAENCHANTERACTION_H
#define PLAYERBOTS_COAENCHANTERACTION_H

#include "Action.h"

#include <ctime>

class PlayerbotAI;

// The enchanter of the team (CoaEnchanter), out of combat: the enchanter disenchants, takes the gear out of the guild
// bank (mode bank) and mails its materials to its master away from a vault (mode mail); the other alts mail it
// their greens (mode mail). The guild bank deposits themselves are the "guild bank deposit" action's.
class CoaEnchanterAction : public Action
{
public:
    CoaEnchanterAction(PlayerbotAI* botAI) : Action(botAI, "coa enchanter") {}

    bool Execute(Event event) override;
    bool isUseful() override;

private:
    bool Disenchant(uint32 skill);
    time_t nextCheck = 0;
    time_t nextMail = 0;
    time_t nextWithdraw = 0;
    time_t rightsToldAt = 0;
};

#endif

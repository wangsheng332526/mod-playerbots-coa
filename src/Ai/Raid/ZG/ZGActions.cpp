/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ZGActions.h"

#include <algorithm>

#include "Playerbots.h"
#include "ZGHelpers.h"

using namespace ZulGurubHelpers;

bool BloodlordMandokirStopCausingThreatAction::Execute(Event /*event*/)
{
    // Auto-attack, Auto Shot and a cast under way; the multiplier blocks new ones.
    bot->AttackStop();
    bot->InterruptNonMeleeSpells(false);
    return true;
}

HakkarAvoidBloodScytheAction::HakkarAvoidBloodScytheAction(PlayerbotAI* botAI)
    : ZulGurubMoveToBossFlankAction(botAI, "hakkar avoid blood scythe", "hakkar", HAKKAR_SCYTHE_FLANK_ANGLE)
{
}

VenoxisAvoidVenomSpitAction::VenoxisAvoidVenomSpitAction(PlayerbotAI* botAI)
    : ZulGurubMoveToBossFlankAction(botAI, "venoxis avoid venom spit", "high priest venoxis", VENOXIS_SPIT_FLANK_ANGLE)
{
}

bool ZulGurubMoveToBossFlankAction::Execute(Event /*event*/)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", bossName);
    if (!boss)
        return false;

    float const distance = std::max(bot->GetDistance2d(boss), bot->GetMeleeRange(boss) * 0.5f);
    Position left = boss->GetPosition();
    Position right = boss->GetPosition();
    left.RelocatePolarOffset(flankAngle, distance);
    right.RelocatePolarOffset(-flankAngle, distance);
    Position const& destination = bot->GetExactDist2d(left) < bot->GetExactDist2d(right) ? left : right;

    float x = destination.GetPositionX();
    float y = destination.GetPositionY();
    float z = boss->GetPositionZ();
    bool const exact = bot->GetMap()->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                                     bot->GetPositionZ(), x, y, z);

    return MoveTo(boss->GetMapId(), x, y, z, false, false, false, exact, MovementPriority::MOVEMENT_COMBAT);
}

/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COAMISSIONACTION_H
#define PLAYERBOTS_COAMISSIONACTION_H

#include "Action.h"
#include "NewRpgBaseAction.h"

class PlayerbotAI;
class Unit;
struct CoaMission;

// "mission gather [minutes] [mining|herbalism|skinning|fishing]", "mission status", "mission stop", whispered to an
// alt by its owner.
class CoaMissionAction : public Action
{
public:
    CoaMissionAction(PlayerbotAI* botAI) : Action(botAI, "mission") {}

    bool Execute(Event event) override;

    // requester null: a mission of the .playerbots missiontest bench, for a random bot.
    // orderItem: "mission order", gather that item until there are orderCount of it.
    // risk: "risk" was asked, a mission in High Risk is accepted.
    bool Start(Player* requester, uint32 minutes, uint32 skillId, std::string& error, uint32 orderItem = 0,
               uint32 orderCount = 0, bool risk = false);
    // "MISSIONCAN;...": the activities this alt can be sent to, for the addon.
    std::string CanLine();
};

// One step of the mission of the "mission" strategy: walk to the gathering spot, go from node to node, end it.
class CoaMissionUpdateAction : public NewRpgBaseAction
{
public:
    CoaMissionUpdateAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "mission update") {}

    bool Execute(Event event) override;
    bool isUseful() override;

private:
    bool DoTravel(CoaMission& mission, uint32 now);
    bool DoWork(CoaMission& mission, uint32 now);
    // Skinning and cloth: the creature to kill next, and the fight itself, left to the combat strategies.
    Unit* FindPrey(CoaMission const& mission);
    bool Hunt(CoaMission& mission, Unit* prey, uint32 now);
    bool DoFish(CoaMission& mission, uint32 now);
    bool FindShore(CoaMission& mission, float wx, float wy, float wz);
    // The spot is done (gathered out, or nothing for long): another one, or the end. true when it decided.
    bool LeaveSpot(CoaMission& mission, uint32 now);
    void TeleportToSpot(CoaMission const& mission);
};

#endif

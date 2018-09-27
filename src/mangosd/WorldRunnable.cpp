/*
 * Copyright (C) 2005-2011 MaNGOS <http://getmangos.com/>
 * Copyright (C) 2009-2011 MaNGOSZero <https://github.com/mangos/zero>
 * Copyright (C) 2011-2016 Nostalrius <https://nostalrius.org>
 * Copyright (C) 2016-2017 Elysium Project <https://github.com/elysium-project>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/** \file
    \ingroup mangosd
*/

#include "WorldSocketMgr.h"
#include "NodesMgr.h"
#include "Common.h"
#include "World.h"
#include "WorldRunnable.h"
#include "Timer.h"
#include "ObjectAccessor.h"
#include "MapManager.h"
#include "BattleGroundMgr.h"
#include "Master.h"

#include "Database/DatabaseEnv.h"

#define WORLD_SLEEP_CONST 50

#ifdef WIN32
#include "ServiceWin32.h"
extern int m_ServiceStatus;
#endif

/// Heartbeat for the World
void WorldRunnable::operator()()
{
    ///- Init new SQL thread for the world database
    WorldDatabase.ThreadStart();                                // let thread do safe mySQL requests (one connection call enough)
    sWorld.InitResultQueue();

    Master::ArmAnticrash();
    uint32 anticrashRearmTimer = 0;

    uint32 realCurrTime = 0;
    uint32 realPrevTime = WorldTimer::tick();

    uint32 prevSleepTime = 0;                               // used for balanced full tick time length near WORLD_SLEEP_CONST

    ///- While we have not World::m_stopEvent, update the world
    while (!World::IsStopped())
    {
        ++World::m_worldLoopCounter;
        realCurrTime = WorldTimer::getMSTime();

        uint32 diff = WorldTimer::tick();
        if (sWorld.getConfig(CONFIG_UINT32_PERFLOG_SLOW_WORLD_UPDATE) && diff > sWorld.getConfig(CONFIG_UINT32_PERFLOG_SLOW_WORLD_UPDATE))
            sLog.out(LOG_PERFORMANCE, "Slow world update: %ums", diff);

        // ANTICRASH
        if (sWorld.GetAnticrashRearmTimer())
        {
            anticrashRearmTimer = sWorld.GetAnticrashRearmTimer();
            sWorld.SetAnticrashRearmTimer(0);
        }
        if (anticrashRearmTimer)
        {
            if (anticrashRearmTimer <= diff)
            {
                anticrashRearmTimer = 0;
                Master::ArmAnticrash();
                sLog.outInfo("Anticrash rearmed");
            }
            else
                anticrashRearmTimer -= diff;
        }

        sWorld.Update(diff);
        realPrevTime = realCurrTime;

        // diff (D0) include time of previous sleep (d0) + tick time (t0)
        // we want that next d1 + t1 == WORLD_SLEEP_CONST
        // we can't know next t1 and then can use (t0 + d1) == WORLD_SLEEP_CONST requirement
        // d1 = WORLD_SLEEP_CONST - t0 = WORLD_SLEEP_CONST - (D0 - d0) = WORLD_SLEEP_CONST + d0 - D0
        if (diff <= WORLD_SLEEP_CONST+prevSleepTime)
        {
            prevSleepTime = WORLD_SLEEP_CONST+prevSleepTime-diff;
            std::this_thread::sleep_for(std::chrono::milliseconds(prevSleepTime));
        }
        else
            prevSleepTime = 0;

        #ifdef WIN32
            if (m_ServiceStatus == 0) World::StopNow(SHUTDOWN_EXIT_CODE);
            while (m_ServiceStatus == 2) Sleep(1000);
        #endif
    }

    sWorld.Shutdown();

    // unload battleground templates before different singletons destroyed
    sBattleGroundMgr.DeleteAllBattleGrounds();

    sWorldSocketMgr->StopNetwork();
    sNodesMgr->OnServerShutdown();

    sMapMgr.UnloadAll();                                    // unload all grids (including locked in memory)

    ///- End the database thread
    WorldDatabase.ThreadEnd();                              // free mySQL thread resources
}

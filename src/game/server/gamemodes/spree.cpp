#include <game/mapitems.h>
#include <game/server/entities/character.h>
#include <game/server/player.h>
#include <game/server/gamecontext.h>
#include "spree.h"

CGameControllerSPREE::CGameControllerSPREE(class CGameContext *pGameServer)
: IGameController(pGameServer)
{
    m_pGameType = "SPREE";
}

void CGameControllerSPREE::Tick()
{

}

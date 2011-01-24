#ifndef GAME_SERVER_GAMEMODES_SPREE_H
#define GAME_SERVER_GAMEMODES_SPREE_H
#include <game/server/gamecontroller.h>
#include <game/server/entity.h>

class CGameControllerSPREE : public IGameController
{
public:
	CGameControllerSPREE(class CGameContext *pGameServer);

	virtual void Tick();
};

#endif



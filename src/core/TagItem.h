#pragma once
#include "RampAgent.h"

using namespace rampAgent;

void RampAgent::RegisterTagItems() {
	RegisterTagItemType("STAND", static_cast<int>(TagItemID::TagItem_STAND));
	RegisterTagItemType("REMARK", static_cast<int>(TagItemID::TagItem_REMARK));
}

inline void rampAgent::RampAgent::RegisterTagActions()
{
	RegisterTagItemFunction("STAND MENU", static_cast<int>(TagActionID::TagAction_OpenMENU));
}

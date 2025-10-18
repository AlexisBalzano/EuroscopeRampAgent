#pragma once
#include "RampAgent.h"

using namespace rampAgent;

void RampAgent::RegisterTagItems() {
	myPluginInstance->RegisterTagItemType("STAND", static_cast<int>(TagItemID::TagItem_STAND));
}

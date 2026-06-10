#include "RampAgent.h"

using namespace rampAgent;

inline void RampAgent::RegisterTagActions()
{
	RegisterTagItemFunction("STAND MENU", static_cast<int>(TagActionID::OpenMENU));
}

inline void RampAgent::OnFunctionCall(int functionId, const char* itemString, POINT pt, RECT area)
{
	std::ignore = pt;
	std::ignore = area;
	std::ignore = itemString;
	std::ignore = functionId;

	//// Check if we're a controller and connected (these are set by OnTimer)
	//if (isController_ == false || isConnected_ == false) return; // If OBS, can't assign stands

	//auto fp = FlightPlanSelectASEL();
	//std::string callsign = toUpper(fp.GetCallsign());
	//std::string icao = toUpper(fp.GetFlightPlanData().GetDestination());

	//if (icao.substr(0, 2) != "LF") {
	//	DisplayMessage("Stand assignment only available for French airports.", "");
	//	return; // Only French airports supported
	//}

	//switch (static_cast<TagActionID>(functionId)) {
	//case TagActionID::OpenMENU:
	//{
	//	OpenPopupList(area, icao.c_str(), 1);

	//	updateStandMenuButtons(icao);

	//	for (const auto& button : menuButtons_) {
	//		AddPopupListElement(button.c_str(), NULL, static_cast<int>(TagActionID::AssignSTAND), false, 2, false, false);
	//	}
	//	AddPopupListElement("None", NULL, static_cast<int>(TagActionID::AssignSTAND), false, 2, false, true);
	//	AddPopupListElement("[---]", NULL, static_cast<int>(TagActionID::AssignSTAND), false, 2, false, true);
	//	break;
	//}
	//case TagActionID::AssignSTAND:
	//{
	//	if (itemString == nullptr || strlen(itemString) == 0) {
	//		DisplayMessage("No stand selected for assignment.", "");
	//		return;
	//	}

	//	if (itemString == std::string("[---]")) {
	//		OpenPopupEdit(area, static_cast<int>(TagActionID::AssignSTAND), "---");
	//		return;
	//	}

	//	// Execute synchronously since this is user-initiated and they expect immediate feedback
	//	// Don't use m_thread as it's reserved for the background worker
	//	assignStandToAircraft(callsign, std::string(itemString), icao);
	//	break;
	//}
	//default:
	//	break;
	//}
}

//void RampAgent::assignStandToAircraft(const std::string& callsign, const std::string& standName, std::string menuIcao)
//{
//	// Queue the assignment request to be processed by worker thread
//	queueApiRequest(ApiRequestType::ASSIGN_STAND, menuIcao, callsign, standName);
//}
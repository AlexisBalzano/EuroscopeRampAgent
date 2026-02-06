#include "RampAgent.h"

using namespace rampAgent;

inline void RampAgent::RegisterTagActions()
{
	RegisterTagItemFunction("STAND MENU", static_cast<int>(TagActionID::OpenMENU));
	RegisterTagItemFunction("ASSIGN STAND", static_cast<int>(TagActionID::AssignSTAND));
}

inline void RampAgent::OnFunctionCall(int functionId, const char* itemString, POINT pt, RECT area)
{
	std::ignore = pt;

	// Check if we're a controller and connected (these are set by OnTimer)
	if (isController_ == false || isConnected_ == false) return; // If OBS, can't assign stands

	auto fp = FlightPlanSelectASEL();
	std::string callsign = toUpper(fp.GetCallsign());
	std::string icao = toUpper(fp.GetFlightPlanData().GetDestination());

	if (icao.substr(0, 2) != "LF") {
		DisplayMessage("Stand assignment only available for French airports.", "");
		return; // Only French airports supported
	}

	switch (static_cast<TagActionID>(functionId)) {
	case TagActionID::OpenMENU:
	{
		OpenPopupList(area, icao.c_str(), 1);

		updateStandMenuButtons(icao);

		for (const auto& button : menuButtons_) {
			AddPopupListElement(button.c_str(), NULL, static_cast<int>(TagActionID::AssignSTAND), false, 2, false, false);
		}
		AddPopupListElement("None", NULL, static_cast<int>(TagActionID::AssignSTAND), false, 2, false, true);
		AddPopupListElement("[---]", NULL, static_cast<int>(TagActionID::AssignSTAND), false, 2, false, true);
		break;
	}
	case TagActionID::AssignSTAND:
	{
		if (itemString == nullptr || strlen(itemString) == 0) {
			DisplayMessage("No stand selected for assignment.", "");
			return;
		}

		if (itemString == std::string("[---]")) {
			OpenPopupEdit(area, static_cast<int>(TagActionID::AssignSTAND), "---");
			return;
		}

		// Execute synchronously since this is user-initiated and they expect immediate feedback
		// Don't use m_thread as it's reserved for the background worker
		assignStandToAircraft(callsign, std::string(itemString), icao);
		break;
	}
	default:
		break;
	}
}

inline void rampAgent::RampAgent::updateStandMenuButtons(const std::string& icao)
{
	menuButtons_.clear();
	
	// Queue request to fetch stands data asynchronously
	queueApiRequest(ApiRequestType::FETCH_STANDS, icao);
	
	// Use cached stands data if available
	nlohmann::ordered_json standsJson;
	{
		std::lock_guard<std::mutex> lock(standsDataCacheMutex_);
		standsJson = standsDataCache_;
	}
	
	// If no cached data yet, show minimal menu
	if (standsJson.empty()) {
		menuButtons_.clear();
		return;
	}

	// Get a copy of assignedStands to check against
	nlohmann::ordered_json assignedStandsCopy;
	{
		std::lock_guard<std::mutex> lock(assignedStandsMutex_);
		assignedStandsCopy = assignedStands_;
	}

	// Check if we have valid assigned stands data
	if (assignedStandsCopy.empty() || 
	    !assignedStandsCopy.contains("assignedStands") || 
	    !assignedStandsCopy.contains("occupiedStands") ||
	    !assignedStandsCopy.contains("blockedStands")) {
		menuButtons_.clear();
		return;
	}

	// deduct available stands list from all stands + occupied stands + blocked stands
	std::vector<std::string> availableStands;

	for (auto& [standName, standData] : standsJson.items()) {
		// Check if stand is already Assigned
		bool isOccupied = false;
		
		for (const auto& occupied : assignedStandsCopy["assignedStands"]) {
			if (occupied.contains("name") && occupied["name"].is_string() && 
			    occupied["name"].get<std::string>() == standName) {
				isOccupied = true;
				break;
			}
		}
		
		if (!isOccupied) {
			// Check if stand is already occupied
			for (const auto& occupied : assignedStandsCopy["occupiedStands"]) {
				if (occupied.contains("name") && occupied["name"].is_string() && 
				    occupied["name"].get<std::string>() == standName) {
					isOccupied = true;
					break;
				}
			}
		}
		
		if (!isOccupied) {
			// Check if stand is blocked
			for (const auto& occupied : assignedStandsCopy["blockedStands"]) {
				if (occupied.contains("name") && occupied["name"].is_string() && 
				    occupied["name"].get<std::string>() == standName) {
					isOccupied = true;
					break;
				}
			}
		}
		
		if (!isOccupied) {
			availableStands.push_back(standName);
		}
	}

	//Sort stands alphabetically -> 2A,2B, 3A,3B,...
	sortStandList(availableStands);
	menuButtons_ = availableStands;
}

void RampAgent::assignStandToAircraft(const std::string& callsign, const std::string& standName, std::string menuIcao)
{
	// Queue the assignment request to be processed by worker thread
	queueApiRequest(ApiRequestType::ASSIGN_STAND, menuIcao, callsign, standName);
}
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

	// Check if we're a controller and connected (these are set by OnTimer)
	//if (!isConnected_.load(std::memory_order_acquire) || !isController_.load(std::memory_order_acquire)) return;

	auto fp = FlightPlanSelectASEL();
	std::string callsign = ToUpper(fp.GetCallsign());
	std::string icao = ToUpper(fp.GetFlightPlanData().GetDestination());

	std::unordered_map<std::string, std::vector<Stand>> localAirportStandsCache;
	{
		std::lock_guard<std::mutex> lock(standsCacheMutex_);
		localAirportStandsCache = airportStandsCache_;
	}

	if (!localAirportStandsCache.contains(icao)) {
		DisplayError(std::format("{} is not supported.", icao));
		return;
	}

	switch (static_cast<TagActionID>(functionId)) {
	case TagActionID::OpenMENU:
	{
		OpenPopupList(area, icao.c_str(), 1);

		for (const auto& stand : airportStandsCache_[icao]) {
			AddPopupListElement(stand.name.c_str(), NULL, static_cast<int>(TagActionID::AssignStand), false, 2, false, false);
		}
		AddPopupListElement("None", NULL, static_cast<int>(TagActionID::AssignStand), false, 2, false, true);
		AddPopupListElement("[---]", NULL, static_cast<int>(TagActionID::AssignStand), false, 2, false, true);
		break;
	}
	case TagActionID::AssignStand:
	{
		if (itemString == nullptr || strlen(itemString) == 0) {
			DisplayMessage("No stand selected for assignment.");
			return;
		}

		// Handle stand freeing
		if (itemString == std::string("None")) {
			// Add request to queue that will be processed by worker thread
			// Clear flight strip annotation immediately for better UX; it will be set again by ES if the API request fails and the stand is still assigned
			CFlightPlanControllerAssignedData assignedData = fp.GetControllerAssignedData();
			assignedData.SetFlightStripAnnotation(STAND_FLIGHT_STRIP_INDEX, ""); // Clear the annotation field on the flight strip immediately for better UX
			assignedData.SetFlightStripAnnotation(REMARK_FLIGHT_STRIP_INDEX, ""); // Clear the annotation field on the flight strip immediately for better UX
			std::lock_guard<std::mutex> lock(apiRequestQueueMutex_);
			pendingAssignRequests_[callsign] = Stand{ .name = "None", .icao = icao, .remark = ""};
			return;
		}

		if (itemString == std::string("[---]")) {
			OpenPopupEdit(area, static_cast<int>(TagActionID::AssignStand), "---");
			return;
		}

		// Handle manual entry
		bool isStandCorrect = false;
		std::vector<Stand> standsAtAirport = localAirportStandsCache[icao];
		for (const auto& stand : standsAtAirport) {
			if (stand.name == itemString) {
				isStandCorrect = true;
				break;
			}
		}
		if (!isStandCorrect) {
			DisplayError("Invalid stand selected: " + std::string(itemString));
			return;
		}

		// Add request to queue that will be processed by worker thread
		std::lock_guard<std::mutex> lock(apiRequestQueueMutex_);
		pendingAssignRequests_[callsign] = Stand{ .name = itemString, .icao = icao, .remark = ""};
		break;
	}
	default:
		break;
	}
}
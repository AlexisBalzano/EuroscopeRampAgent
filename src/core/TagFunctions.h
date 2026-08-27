#include "RampAgent.h"
#include "Helpers.h"

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

	// vSMR calls into this from its own OnClickScreenObject, and the aircraft it right
	// clicked may have no flight plan at all, in which case its SetASELAircraft was a
	// no-op and the selection here is stale or empty.
	auto fp = FlightPlanSelectASEL();
	if (fp.IsValid() == false) return;

	std::string callsign = ToUpper(SafeString(fp.GetCallsign()));
	std::string icao = ToUpper(SafeString(fp.GetFlightPlanData().GetDestination()));
	if (callsign.empty() || icao.empty()) return; // Nothing to assign a stand against

	// Copy just this airport's stands under the lock, rather than the whole cache. Note
	// find() and not operator[]: the latter inserts an empty entry for an unsupported
	// ICAO, which can rehash the map while the worker thread is populating it.
	std::vector<Stand> airportStands;
	bool airportSupported = false;
	{
		std::lock_guard<std::mutex> lock(standsCacheMutex_);
		if (const auto airport = airportStandsCache_.find(icao); airport != airportStandsCache_.end()) {
			airportStands = airport->second;
			airportSupported = true;
		}
	}

	// Reported after the lock is released, since DisplayError calls into Euroscope
	if (airportSupported == false) {
		DisplayError(std::format("{} is not supported.", icao));
		return;
	}

	switch (static_cast<TagActionID>(functionId)) {
	case TagActionID::OpenMENU:
	{
		OpenPopupList(area, icao.c_str(), 1);

		for (const auto& stand : airportStands) {
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
		for (const auto& stand : airportStands) {
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
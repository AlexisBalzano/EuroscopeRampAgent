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

	if (canSendReport_ == false || isConnected_ == false) return; // If OBS, can't assign stands

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

		AddPopupListElement("None", NULL, static_cast<int>(TagActionID::AssignSTAND), false, 2, false, false);
		for (const auto& button : menuButtons_) {
			AddPopupListElement(button.c_str(), NULL, static_cast<int>(TagActionID::AssignSTAND), false, 2, false, false);
		}
		break;
	}
	case TagActionID::AssignSTAND:
	{
		if (m_thread.joinable()) {
			m_thread.join();
		}
		m_thread = std::thread(&RampAgent::assignStandToAircraft, this, callsign, std::string(itemString), icao);
		break;
	}
	default:
		break;
	}
}

inline void rampAgent::RampAgent::updateStandMenuButtons(const std::string& icao)
{
	menuButtons_.clear();
	nlohmann::ordered_json standsJson = nlohmann::ordered_json::object();

	httplib::SSLClient cli(apiUrl_);
	cli.set_connection_timeout(0, 700000); // 700ms
	cli.set_read_timeout(1, 0);            // 1s
	cli.set_write_timeout(1, 0);           // 1s
	httplib::Headers headers = { {"User-Agent", "EuroscopeRampAgent"} };
	std::string apiEndpoint = "/rampagent/api/airports/" + icao + "/stands";

	auto res = cli.Get(apiEndpoint.c_str(), headers);

	if (res && res->status >= 200 && res->status < 300) {
		if (!printError) {
			printError = true; // reset error printing flag on success
			DisplayMessage("Successfully retrieved stands information from NeoRampAgent server for airport " + icao, "");
		}
		try {
			if (!res->body.empty()) standsJson = nlohmann::ordered_json::parse(res->body);
		}
		catch (const std::exception& e) {
			DisplayMessage("Failed to parse stands data from NeoRampAgent server: " + std::string(e.what()), "");
			return;
		}
	}
	else {
		if (printError) {
			printError = false; // avoid spamming logs
			DisplayMessage("Failed to get stands information from NeoRampAgent server. HTTP status: " + std::to_string(res ? res->status : 0), "");
		}
		return;
	}

	if (standsJson.empty()) {
		if (printError) {
			printError = false; // avoid spamming logs
			DisplayMessage("No stands data received from NeoRampAgent server for airport " + icao, "");
		}
		return;
	}

	// deduct available stands list from all stands + occupied stands + blocked stands
	std::vector<std::string> availableStands;
	{
		std::lock_guard<std::mutex> lock(assignedStandsMutex_);

		for (auto& [standName, standData] : standsJson.items()) {
			// Check if stand is already Assigned
			bool isOccupied = false;
			for (const auto& occupied : assignedStands_["assignedStands"]) {
				if (occupied["name"].get<std::string>() == standName) {
					isOccupied = true;
					break;
				}
			}
			// Check if stand is already occupied
			for (const auto& occupied : assignedStands_["occupiedStands"]) {
				if (occupied["name"].get<std::string>() == standName) {
					isOccupied = true;
					break;
				}
			}
			// Check if stand is blocked
			for (const auto& occupied : assignedStands_["blockedStands"]) {
				if (occupied["name"].get<std::string>() == standName) {
					isOccupied = true;
					break;
				}
			}
			if (!isOccupied) {
				availableStands.push_back(standName);
			}
		}
	}

	//Sort stands alphabetically -> 2A,2B, 3A,3B,...
	sortStandList(availableStands);
	menuButtons_ = availableStands;
}

void RampAgent::assignStandToAircraft(const std::string& callsign, const std::string& standName, std::string menuIcao)
{
	std::string token = generateToken(callsign_);

	httplib::SSLClient cli(apiUrl_);
	cli.set_connection_timeout(0, 700000); // 700ms
	cli.set_read_timeout(1, 0);            // 1s
	cli.set_write_timeout(1, 0);           // 1s
	httplib::Headers headers = { {"User-Agent", "EuroscopeRampAgent"} };
	std::string apiEndpoint = "/rampagent/api/assign?stand=" + standName + "&icao=" + menuIcao + "&callsign=" + callsign + "&token=" + token + "&client=" + callsign_;

	auto res = cli.Get(apiEndpoint.c_str(), headers);

	if (!res || !(res->status >= 200 && res->status < 300)) {
		queueMessage("Failed to send manual assign to NeoRampAgent server. HTTP status: " + std::to_string(res ? res->status : 0));
		return;
	}
	else { // assignement processed, check response to see if successful and update tag item if so
		if (!res->body.empty()) {
			nlohmann::ordered_json dataJson = nlohmann::ordered_json::parse(res->body);
			if (!dataJson.contains("message")) return; // malformed response
			if (dataJson["message"]["action"].get<std::string>() == "assign") {
				{
					std::lock_guard<std::mutex> lock(lastStandTagMapMutex_);
					lastStandTagMap_[callsign] = standName;
				}
				{
					std::lock_guard<std::mutex> lock(manualAssignedCallsignsMutex_);
					manualAssignedCallsigns_[callsign] = standName;
				}
				UpdateTagItems(callsign, WHITE, standName);
				return;
			}
			else if (dataJson["message"]["action"].get<std::string>() == "free") {
				{
					std::lock_guard<std::mutex> lock(lastStandTagMapMutex_);
					lastStandTagMap_.erase(callsign);
				}
				{
					std::lock_guard<std::mutex> lock(manualAssignedCallsignsMutex_);
					manualAssignedCallsigns_[callsign] = "";
				}
				UpdateTagItems(callsign, WHITE, "");
				return;
			}
			else {
				queueMessage("Manual stand rejected: " + dataJson["message"]["message"].get<std::string>());
				DisplayMessage("Manual stand rejected: " + dataJson["message"]["message"].get<std::string>());
				return;
			}
		}
	}
	queueMessage("Manual stand assignment failed for " + callsign + " to " + standName);
}
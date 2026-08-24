#include <numeric>
#include <algorithm>
#include <limits>
#include <cctype>
#include <memory>
#include <httplib.h>
#include <fstream>
#include <filesystem>
#include <openssl/sha.h>

#include "RampAgent.h"
#include "version.h"
#include "core/TagItem.h"
#include "core/CompileCommands.h"
#include "core/TagFunctions.h"
#include "Secret.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

using namespace rampAgent;
using namespace EuroScopePlugIn;

std::unique_ptr<rampAgent::RampAgent> myPluginInstance = nullptr;

RampAgent::RampAgent() : CPlugIn(EuroScopePlugIn::COMPATIBILITY_CODE, "RampAgent", PLUGIN_VERSION, "French vACC", "Open Source")
{
	m_stop.store(false, std::memory_order_relaxed);
	Initialize();
};
RampAgent::~RampAgent()
{
	Shutdown();
};


void __declspec (dllexport) EuroScopePlugInInit(EuroScopePlugIn::CPlugIn** ppPlugInInstance)
{
	myPluginInstance.reset();
	myPluginInstance = std::make_unique<RampAgent>();
	*ppPlugInInstance = myPluginInstance.get();
}


void __declspec (dllexport) EuroScopePlugInExit()
{
	myPluginInstance.reset();
}

void RampAgent::Initialize()
{
	try
	{
		initialized_ = true;
		RegisterTagItems();
		RegisterTagActions();
		
		// Start the persistent worker thread
		m_stop.store(false, std::memory_order_release);
		m_thread = std::thread(&RampAgent::WorkerThread, this);
	}
	catch (const std::exception& e)
	{
		DisplayError("Failed to initialize Ramp Agent: " + std::string(e.what()));
	}
}

void RampAgent::Shutdown()
{
	if (initialized_)
	{
		initialized_ = false;
	}
	
	// Signal worker thread to stop with proper memory ordering
	m_stop.store(true, std::memory_order_release);
	
	// Wait for worker thread to finish
	if (m_thread.joinable())
		m_thread.join();

	DisplayMessage("Ramp Agent shutdown complete");
}

void RampAgent::DisplayMessage(const std::string& message) {
	DisplayUserMessage("Ramp Agent", "", message.c_str(), true, true, false, false, false);
}

void RampAgent::DisplayError(const std::string& message)
{
	DisplayUserMessage("Ramp Agent", "ERROR", message.c_str(), true, true, true, true, true);
}

void RampAgent::QueueError(const std::string& message)
{
	std::lock_guard<std::mutex> lock(messageQueueMutex_);
	messageQueue_.push_back({message, true});
}

void RampAgent::QueueMessage(const std::string& message)
{
	std::lock_guard<std::mutex> lock(messageQueueMutex_);
	messageQueue_.push_back({message, false});
}

void RampAgent::WorkerThread() {
	// Create a single SSL client instance for all API calls
	auto cli = std::make_unique<httplib::SSLClient>(API_URL, 443);
	cli->set_connection_timeout(0, 2000000); // 2s
	cli->set_read_timeout(1, 0);             // 1s
	cli->set_write_timeout(1, 0);            // 1s

	PopulateICAOStandMap(*cli);

	while (m_stop.load(std::memory_order_acquire) == false) {
		static size_t counter = 0;


		std::string userCallsign;
		{
			std::lock_guard<std::mutex> lock(userCallsignMutex_);
			userCallsign = userCallsign_;
		}

		// Fetch all stand periodically and update standsCache_
		if (isConnected_.load(std::memory_order_acquire) && counter % (PERIODIC_FETCH_TIME_INTERVAL * 10) == 0) { // every PERIODIC_FETCH_TIME_INTERVAL seconds
			FetchAndUpdateAssignedStands(*cli, userCallsign);
		}


		// Send assigned stand if controller
		if (isController_.load(std::memory_order_acquire)){
			std::lock_guard<std::mutex> lock(apiRequestQueueMutex_);
			if (pendingAssignRequests_.empty() == false) {
				for (const auto& [callsign, standInfo] : pendingAssignRequests_) {
					SendStandAssignementRequest(*cli, userCallsign, callsign, standInfo);
				}
				pendingAssignRequests_.clear();
			}
		}

		++counter;
		std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Avoid busy waiting
	}
}

void RampAgent::FetchAndUpdateAssignedStands(httplib::SSLClient& cli, const std::string& userCallsign)
{
	httplib::Headers headers = { {"User-Agent", "EuroscopeRampAgent"} };
	auto res = cli.Get("/api/occupancy/?callsign=" + userCallsign, headers);

	nlohmann::ordered_json response;

	if (res && res->status >= 200 && res->status < 300) {
		if (!printError) { // reset error printing flag on success 
			QueueMessage("Successfully reconnected to Ramp Agent server.");
			printError = true;
		}
		try {
			if (!res->body.empty()) response = nlohmann::ordered_json::parse(res->body);
			else {
				QueueError("Received empty response from Ramp Agent server.");
				return;
			}
		}
		catch (const nlohmann::json::exception& e) {
			QueueError("Failed to parse occupied stands data from Ramp Agent server: " + std::string(e.what()));
			return;
		}
		catch (const std::exception& e) {
			QueueError("Failed to parse occupied stands data from Ramp Agent server: " + std::string(e.what()));
			return;
		}
	}
	else {
		if (printError) {
			printError = false;
			QueueError("Failed to retrieve occupied stands data from Ramp Agent server. HTTP status: " + std::to_string(res ? res->status : 0));
			if (!res) {
				QueueError("Error details: " + httplib::to_string(res.error()));
			}
		}
		return;
	}

	// Parse response and update airportStandsCache_
	try {
		if (!response.contains("assignedStands") || !response["assignedStands"].is_array()) {
			QueueError("Invalid API response: missing or invalid 'assignedStands' field");
			return;
		}

		if (!response.contains("occupiedStands") || !response["occupiedStands"].is_array()) {
			QueueError("Invalid API response: missing or invalid 'occupiedStands' field");
			return;
		}

		auto& assigned = response["assignedStands"];

		// Merge occupied stands into assigned stands
		for (auto& occupiedStand : response["occupiedStands"]) {
			assigned.push_back(occupiedStand);
		}

		std::unordered_map<std::string, Stand> standTagMap;

		for (auto& stand : assigned) {
			if (!stand.is_object()) continue;

			// callsign
			const auto csIt = stand.find("callsign");
			if (csIt == stand.end() || !csIt->is_string()) continue;
			const std::string callsign = csIt->get<std::string>();

			// icao
			const auto icaoIt = stand.find("icao");
			if (icaoIt == stand.end() || !icaoIt->is_string()) continue;
			std::string icao = icaoIt->get<std::string>();

			// stand name
			const auto nameIt = stand.find("name");
			if (nameIt == stand.end() || !nameIt->is_string()) continue;
			std::string standName = nameIt->get<std::string>();

			// remark: accept string only; treat null/other as empty
			std::string remark;
			if (auto rIt = stand.find("remark"); rIt != stand.end() && rIt->is_string()) {
				remark = rIt->get<std::string>();
			}
			else {
				remark.clear();
			}

			Stand standInfo{ .name = standName, .icao = icao, .remark = remark };

			standTagMap[callsign] = standInfo;
		}
		
		std::lock_guard<std::mutex> lock(standsCacheMutex_);
		standsCache_ = std::move(standTagMap);
	}
	catch (const nlohmann::json::exception& e) {
		QueueError("runScopeUpdate: failed to process assigned stands: " + std::string(e.what()));
		return;
	}
	catch (const std::exception& e) {
		QueueError("runScopeUpdate: failed to process assigned stands: " + std::string(e.what()));
		return;
	}
}

void rampAgent::RampAgent::PopulateICAOStandMap(httplib::SSLClient& cli)
{
	httplib::Headers headers = { {"User-Agent", "EuroscopeRampAgent"} };
	auto res = cli.Get("/api/airports/", headers);

	nlohmann::ordered_json response;

	if (res && res->status >= 200 && res->status < 300) {
		try {
			if (!res->body.empty()) response = nlohmann::ordered_json::parse(res->body);
			else {
				QueueError("Received empty response from Ramp Agent server when parsing compatible airports.");
				return;
			}
		}
		catch (const nlohmann::json::exception& e) {
			QueueError("Failed to parse compatible airports data from Ramp Agent server: " + std::string(e.what()));
			return;
		}
		catch (const std::exception& e) {
			QueueError("Failed to parse compatible airports data from Ramp Agent server: " + std::string(e.what()));
			return;
		}
	}
	else {
		QueueError("Failed to retrieve compatible airports data from Ramp Agent server. HTTP status: " + std::to_string(res ? res->status : 0));
		if (!res) {
			QueueError("Error details: " + httplib::to_string(res.error()));
		}
		return;
	}

	// Parse response to get list of compatible airports
	std::vector<std::string> compatibleAirports;
	try {
		if (!response.is_array()) {
			QueueError("Invalid API response: response is not an array");
			return;
		}
		

		for (const auto& airport : response) {
			if (!airport.is_object()) continue;

			// icao
			const auto nameIt = airport.find("name");
			if (nameIt == airport.end() || !nameIt->is_string()) continue;
			std::string name = nameIt->get<std::string>();
			compatibleAirports.push_back(name);
		}
	}
	catch (const nlohmann::json::exception& e) {
		QueueError("runScopeUpdate: failed to process compatible airports: " + std::string(e.what()));
		return;
	}
	catch (const std::exception& e) {
		QueueError("runScopeUpdate: failed to process compatible airports: " + std::string(e.what()));
		return;
	}


	// For each compatible airport, fetch the stands and populate airportStandsCache_
	for (const auto& icao : compatibleAirports) {
		auto res = cli.Get(("/api/airports/" + icao + "/stands").c_str(), headers);
		if (res && res->status >= 200 && res->status < 300) {
			try {
				if (!res->body.empty()) {
					nlohmann::ordered_json standsJson = nlohmann::ordered_json::parse(res->body);
					std::vector<Stand> stands;
					for (const auto& [standName, standInfo] : standsJson.items()) {
						if (!standInfo.is_object()) continue;
						stands.push_back(Stand{ .name = standName, .icao = icao, .remark = "" });
					}
					std::lock_guard<std::mutex> lock(standsCacheMutex_);
					airportStandsCache_[icao] = std::move(stands);
				}
				else {
					QueueError("Received empty response from Ramp Agent server when fetching stands for airport " + icao);
				}
			}
			catch (const nlohmann::json::exception& e) {
				QueueError("Failed to parse stands data for airport " + icao + " from Ramp Agent server: " + std::string(e.what()));
			}
			catch (const std::exception& e) {
				QueueError("Failed to parse stands data for airport " + icao + " from Ramp Agent server: " + std::string(e.what()));
			}
		}
		else {
			QueueError("Failed to retrieve stands data for airport " + icao + " from Ramp Agent server. HTTP status: " + std::to_string(res ? res->status : 0));
			if (!res) {
				QueueError("Error details: " + httplib::to_string(res.error()));
			}
		}
	}
}

void RampAgent::SendStandAssignementRequest(httplib::SSLClient& cli, const std::string& userCallsign, const std::string& callsign, const Stand& standInfo)
{
	httplib::Headers headers = { {"User-Agent", "EuroscopeRampAgent"} };
	
	std::string token = GenerateToken(userCallsign);
	std::string apiEndpoint = "/api/assign?stand=" + standInfo.name +
		"&icao=" + standInfo.icao +
		"&callsign=" + callsign +
		"&token=" + token +
		"&client=" + userCallsign;
	
	auto res = cli.Get(apiEndpoint.c_str(), headers);
	
	if (!res || !(res->status >= 200 && res->status < 300)) {
		QueueError("Failed to send manual assign. HTTP status: " + std::to_string(res ? res->status : 0));
		return;
	}
	
	if (!res->body.empty()) {
		try {
			nlohmann::ordered_json dataJson = nlohmann::ordered_json::parse(res->body);

			if (!dataJson.contains("message")) {
				QueueMessage("Malformed response from server");
				return;
			}

			auto& message = dataJson["message"];
			if (!message.contains("action") || !message["action"].is_string()) {
				QueueMessage("Malformed response from server");
				return;
			}

			std::string action = message["action"].get<std::string>();

			if (action == "assign") {
				// Update local cache to reflect the new assignment
				std::lock_guard<std::mutex> lock(standsCacheMutex_);
				standsCache_[callsign] = standInfo;
				QueueMessage("Stand " + standInfo.name + " assigned to " + callsign);
			}
			else if (action == "free") {
				{
					std::lock_guard<std::mutex> lock(standsCacheMutex_);
					standsCache_.erase(callsign);
				}
				QueueMessage("Stand freed for " + callsign);
			}
			else {
				if (message.contains("message") && message["message"].is_string()) {
					std::string msg = message["message"].get<std::string>();
					QueueMessage("Manual stand rejected: " + msg);
				}
				else {
					QueueMessage("Manual stand assignment failed with unknown action: " + action);
				}
			}
		}
		catch (const nlohmann::json::exception& e) {
			QueueMessage("Failed to parse stand assignment response: " + std::string(e.what()));
		}
		catch (const std::exception& e) {
			QueueMessage("Failed to parse stand assignment response: " + std::string(e.what()));
		}
	}
}

void RampAgent::OnTimer(int Counter) {
	// Update user state
	bool connected = IsConnected();
	isConnected_.store(connected, std::memory_order_release);
	bool controller = IsController();
	isController_.store(controller, std::memory_order_release);
	
	// Display queued messages from worker thread
	{
		std::lock_guard<std::mutex> lock(messageQueueMutex_);
		for (const auto& [msg, isError] : messageQueue_) {
			if (isError) DisplayError(msg);
			else DisplayMessage(msg);
		}
		messageQueue_.clear();
	}

	// Update flight strip annotations (done here to be independant from tag items)
	UpdateFlightStripAnnotations();
}

bool RampAgent::IsConnected()
{
	bool userIsConnected = this->GetConnectionType() == EuroScopePlugIn::CONNECTION_TYPE_DIRECT;
	return userIsConnected;
}

void rampAgent::RampAgent::UpdateFlightStripAnnotations()
{
	std::lock_guard<std::mutex> lock(standsCacheMutex_);
	CRadarTarget rt = this->RadarTargetSelectFirst();
	while (rt.IsValid()) {
		bool isOnGround = rt.IsValid() && rt.GetGS() <= ON_GROUND_SPEED_THRESHOLD; // Only update strip if target is on the ground
		if (!isOnGround) {
			rt = this->RadarTargetSelectNext(rt);
			continue;
		}


		CFlightPlan fp = rt.GetCorrelatedFlightPlan();
		if (fp.IsValid() == false) {
			rt = this->RadarTargetSelectNext(rt);
			continue;
		}

		bool hasStand = standsCache_.contains(rt.GetCallsign()); // Only update if information available

		CFlightPlanControllerAssignedData assignedData = fp.GetControllerAssignedData();
		const std::string standName = hasStand ? standsCache_[rt.GetCallsign()].name : "";
		std::string truncatedStand = standName.length() > 23 ? standName.substr(0, 23) : standName; // Truncate to 23 characters to ensure it fits in the annotation field

		// Only update if value different to avoid unnecessary controllerAssignedData updates
		if (assignedData.GetFlightStripAnnotation(STAND_FLIGHT_STRIP_INDEX) != truncatedStand) {
			assignedData.SetFlightStripAnnotation(STAND_FLIGHT_STRIP_INDEX, truncatedStand.c_str());
		}
		
		const std::string remark = hasStand ? standsCache_[rt.GetCallsign()].remark : "";
		std::string truncatedRemark = remark.length() > 23 ? remark.substr(0, 23) : remark; // Truncate to 23 characters to ensure it fits in the annotation field
		// Only update if value different to avoid unnecessary controllerAssignedData updates
		if (assignedData.GetFlightStripAnnotation(REMARK_FLIGHT_STRIP_INDEX) != truncatedRemark) {
			assignedData.SetFlightStripAnnotation(REMARK_FLIGHT_STRIP_INDEX, truncatedRemark.c_str());
		}

		rt = this->RadarTargetSelectNext(rt);
	}
}

bool RampAgent::IsController()
{
	const std::string callsign = this->ControllerMyself().GetCallsign();
	if (callsign.size() < 3) return false;

	bool userIsObserver = callsign.substr(callsign.size() - 3) == "OBS" || this->ControllerMyself().GetFacility() == 0;
	
	std::lock_guard<std::mutex> lock(userCallsignMutex_);
	userCallsign_ = callsign; // Lock is held by calling function
	
	return !userIsObserver;
}

const std::string RampAgent::GenerateToken(const std::string& callsign)
{
	std::string s = AUTH_SECRET + callsign;
	unsigned char hash[SHA256_DIGEST_LENGTH];
	SHA256(reinterpret_cast<const unsigned char*>(s.data()), s.size(), hash);
	std::ostringstream oss;
	oss << std::hex << std::setfill('0');
	for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
		oss << std::setw(2) << static_cast<int>(hash[i]);
	}
	return oss.str();
}
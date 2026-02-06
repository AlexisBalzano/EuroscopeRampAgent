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

rampAgent::RampAgent* rampAgent::myPluginInstance = nullptr;

RampAgent::RampAgent() : CPlugIn(EuroScopePlugIn::COMPATIBILITY_CODE, "RampAgent", PLUGIN_VERSION, "French vACC", "Open Source"), m_stop(false)
{
	Initialize();
};
RampAgent::~RampAgent()
{
	Shutdown();
};


void __declspec (dllexport) EuroScopePlugInInit(EuroScopePlugIn::CPlugIn** ppPlugInInstance)
{
	*ppPlugInInstance = myPluginInstance = new rampAgent::RampAgent();
}


void __declspec (dllexport) EuroScopePlugInExit()
{
	delete myPluginInstance;
}

void RampAgent::Initialize()
{
	try
	{
		initialized_ = true;
		RegisterTagItems();
		RegisterTagActions();
		
		// Start the persistent worker thread
		m_stop = false;
		m_thread = std::thread(&RampAgent::workerThread, this);
	}
	catch (const std::exception& e)
	{
		DisplayMessage("Failed to initialize Ramp Agent: " + std::string(e.what()), "Error");
	}
}

void RampAgent::Shutdown()
{
	if (initialized_)
	{
		initialized_ = false;
	}
	m_stop = true;
	
	// Wake up the worker thread so it can exit
	m_cv.notify_one();
	
	if (m_thread.joinable())
		m_thread.join();

	DisplayMessage("Ramp Agent shutdown complete", "Status");
}

void RampAgent::Reset()
{
}

void RampAgent::DisplayMessage(const std::string& message, const std::string& sender) {
	DisplayUserMessage("Ramp Agent", sender.c_str(), message.c_str(), true, true, false, false, false);
}

void rampAgent::RampAgent::queueMessage(const std::string& message)
{
	std::lock_guard<std::mutex> lock(messageQueueMutex_);
	messageQueue_.push_back(message);
}

void RampAgent::runUpdate() {
	if (!isConnected_) {
		return;
	}

	// Signal the worker thread to fetch data
	m_fetchRequested.store(true);
	m_cv.notify_one();

	// Now use the existing assignedStands_ data (may be from previous fetch)
	nlohmann::ordered_json assignedStandsCopy;
	{
		std::lock_guard<std::mutex> lock(assignedStandsMutex_);
		assignedStandsCopy = assignedStands_;
	}

	std::unordered_map<std::string, std::string> lastStandTagMapCopy;
	{
		std::lock_guard<std::mutex> lock(lastStandTagMapMutex_);
		lastStandTagMapCopy = lastStandTagMap_;
	}

	if (assignedStandsCopy.empty()) {
		if (printError.exchange(false)) { // avoid spamming logs
			if (!firstTime.exchange(false)) {
				DisplayMessage("No assigned stands data received to update tags.", "");
			}
		}
		// Clear All Tag Items
		for (const auto& [callsign, standName] : lastStandTagMapCopy) {
			UpdateTagItems(callsign, WHITE, "");
		}
		std::lock_guard<std::mutex> lock(lastStandTagMapMutex_);
		lastStandTagMap_.clear();
		return;
	}

	std::unordered_map<std::string, std::string> standTagMap;

	try {
		// Safely check if required keys exist
		if (!assignedStandsCopy.contains("assignedStands") || !assignedStandsCopy["assignedStands"].is_array()) {
			DisplayMessage("Invalid API response: missing or invalid 'assignedStands' field", "Error");
			return;
		}
		
		if (!assignedStandsCopy.contains("occupiedStands") || !assignedStandsCopy["occupiedStands"].is_array()) {
			DisplayMessage("Invalid API response: missing or invalid 'occupiedStands' field", "Error");
			return;
		}

		auto& assigned = assignedStandsCopy["assignedStands"];
		
		// Merge occupied stands into assigned stands
		for (auto& occupiedStand : assignedStandsCopy["occupiedStands"]) {
			assigned.push_back(occupiedStand);
		}

		for (auto& stand : assigned) {
			if (!stand.is_object()) continue;

			// callsign
			const auto csIt = stand.find("callsign");
			if (csIt == stand.end() || !csIt->is_string()) continue;
			const std::string callsign = csIt->get<std::string>();

			if (aircraftExists(callsign).first == false) {
				continue; // Aircraft not found, skip
			}

			// stand name
			const auto nameIt = stand.find("name");
			if (nameIt == stand.end() || !nameIt->is_string()) continue;
			std::string standName = nameIt->get<std::string>();
			standTagMap[callsign] = standName;

			{
				std::lock_guard<std::mutex> lock(manualAssignedCallsignsMutex_);
				if (manualAssignedCallsigns_.find(callsign) != manualAssignedCallsigns_.end()) {
					standName = manualAssignedCallsigns_[callsign];
				}
			}

			standTagMap[callsign] = standName;

			// remark: accept string only; treat null/other as empty
			std::string remark;
			if (auto rIt = stand.find("remark"); rIt != stand.end() && rIt->is_string()) {
				remark = rIt->get<std::string>();
			}
			else {
				remark.clear();
			}

			// Update only if changed or new - now using the copy which is safe
			if (auto it = lastStandTagMapCopy.find(callsign);
				it != lastStandTagMapCopy.end() && it->second == standName) {
				UpdateTagItems(callsign, WHITE, standName, remark);
			}
			else {
				UpdateTagItems(callsign, YELLOW, standName, remark);
			}
		}
	}
	catch (const std::exception& e) {
		DisplayMessage("runScopeUpdate: failed to process assigned stands: " + std::string(e.what()), "Error");
		return;
	}

	// Clear tags for aircraft that are no longer assigned
	for (const auto& [callsign, standName] : lastStandTagMapCopy) {
		if (standTagMap.find(callsign) == standTagMap.end()) {
			UpdateTagItems(callsign, WHITE, "");
		}
	}

	{
		std::lock_guard<std::mutex> lock(manualAssignedCallsignsMutex_);
		manualAssignedCallsigns_.clear();

	}

	std::lock_guard<std::mutex> lock(lastStandTagMapMutex_);
	lastStandTagMap_ = standTagMap;
}

void RampAgent::workerThread() {
	// Create a single SSL client instance for all API calls
	std::string localApiUrl;
	{
		std::lock_guard<std::mutex> lock(apiUrlMutex_);
		localApiUrl = apiUrl_;
	}
	
	// Use unique_ptr so we can recreate the client when URL changes
	auto cli = std::make_unique<httplib::SSLClient>(localApiUrl, 443);
	cli->set_connection_timeout(0, 700000); // 700ms
	cli->set_read_timeout(1, 0);            // 1s
	cli->set_write_timeout(1, 0);           // 1s
	
	while (!m_stop) {
		// Wait for a fetch request or shutdown signal
		{
			std::unique_lock<std::mutex> lock(m_cvMutex);
			m_cv.wait(lock, [this] { 
				return m_fetchRequested.load() || !apiRequestQueue_.empty() || m_stop; 
			});
			
			if (m_stop) {
				break;
			}
			
			// Process periodic fetch if requested
			if (m_fetchRequested.exchange(false)) {
				getAllAssignedStands();
			}
		}
		
		// Process all queued API requests
		while (true) {
			ApiRequest request;
			{
				std::lock_guard<std::mutex> lock(apiRequestQueueMutex_);
				if (apiRequestQueue_.empty()) {
					break;
				}
				request = apiRequestQueue_.front();
				apiRequestQueue_.pop();
			}
			
			// Recreate client if API URL changed
			{
				std::lock_guard<std::mutex> lock(apiUrlMutex_);
				if (localApiUrl != apiUrl_) {
					localApiUrl = apiUrl_;
					cli = std::make_unique<httplib::SSLClient>(localApiUrl, 443);
					cli->set_connection_timeout(0, 700000);
					cli->set_read_timeout(1, 0);
					cli->set_write_timeout(1, 0);
				}
			}
			
			// Process the request
			processApiRequest(*cli, request);
		}
	}
}

void RampAgent::queueApiRequest(ApiRequestType type, const std::string& icao, const std::string& callsign, const std::string& standName) {
	ApiRequest request;
	request.type = type;
	request.icao = icao;
	request.callsign = callsign;
	request.standName = standName;
	
	{
		std::lock_guard<std::mutex> lock(apiRequestQueueMutex_);
		apiRequestQueue_.push(request);
	}
	
	// Wake up worker thread
	m_cv.notify_one();
}

void RampAgent::processApiRequest(httplib::SSLClient& cli, const ApiRequest& request) {
	httplib::Headers headers = { {"User-Agent", "EuroscopeRampAgent"} };
	
	switch (request.type) {
		case ApiRequestType::FETCH_OCCUPANCY:
		{
			// This is handled by getAllAssignedStands() which is already implemented
			break;
		}
		
		case ApiRequestType::FETCH_STANDS:
		{
			std::string apiEndpoint = "/api/airports/" + request.icao + "/stands";
			auto res = cli.Get(apiEndpoint.c_str(), headers);
			
			if (res && res->status >= 200 && res->status < 300) {
				try {
					if (!res->body.empty()) {
						nlohmann::ordered_json standsJson = nlohmann::ordered_json::parse(res->body);
						
						// Cache the stands data
						{
							std::lock_guard<std::mutex> lock(standsDataCacheMutex_);
							standsDataCache_ = standsJson;
						}
						
						if (!printError.load()) {
							printError.store(true);
							queueMessage("Successfully retrieved stands information for " + request.icao);
						}
					}
				}
				catch (const std::exception& e) {
					queueMessage("Failed to parse stands data: " + std::string(e.what()));
				}
			}
			else {
				if (printError.load()) {
					printError.store(false);
					queueMessage("Failed to get stands information. HTTP status: " + std::to_string(res ? res->status : 0));
				}
			}
			break;
		}
		
		case ApiRequestType::ASSIGN_STAND:
		{
			std::string localCallsign;
			{
				std::lock_guard<std::mutex> lock(callsignMutex_);
				localCallsign = callsign_;
			}
			
			std::string token = generateToken(localCallsign);
			std::string apiEndpoint = "/api/assign?stand=" + request.standName + 
			                          "&icao=" + request.icao + 
			                          "&callsign=" + request.callsign + 
			                          "&token=" + token + 
			                          "&client=" + localCallsign;
			
			auto res = cli.Get(apiEndpoint.c_str(), headers);
			
			if (!res || !(res->status >= 200 && res->status < 300)) {
				queueMessage("Failed to send manual assign. HTTP status: " + std::to_string(res ? res->status : 0));
				return;
			}
			
			if (!res->body.empty()) {
				try {
					nlohmann::ordered_json dataJson = nlohmann::ordered_json::parse(res->body);
					
					if (!dataJson.contains("message")) {
						queueMessage("Malformed response from server");
						return;
					}
					
					auto& message = dataJson["message"];
					if (!message.contains("action") || !message["action"].is_string()) {
						queueMessage("Malformed response from server");
						return;
					}
					
					std::string action = message["action"].get<std::string>();
					
					if (action == "assign") {
						{
							std::lock_guard<std::mutex> lock(lastStandTagMapMutex_);
							lastStandTagMap_[request.callsign] = request.standName;
						}
						{
							std::lock_guard<std::mutex> lock(manualAssignedCallsignsMutex_);
							manualAssignedCallsigns_[request.callsign] = request.standName;
						}
						UpdateTagItems(request.callsign, WHITE, request.standName);
						queueMessage("Stand " + request.standName + " assigned to " + request.callsign);
					}
					else if (action == "free") {
						{
							std::lock_guard<std::mutex> lock(lastStandTagMapMutex_);
							lastStandTagMap_.erase(request.callsign);
						}
						{
							std::lock_guard<std::mutex> lock(manualAssignedCallsignsMutex_);
							manualAssignedCallsigns_[request.callsign] = "";
						}
						UpdateTagItems(request.callsign, WHITE, "");
						queueMessage("Stand freed for " + request.callsign);
					}
					else {
						if (message.contains("message") && message["message"].is_string()) {
							std::string msg = message["message"].get<std::string>();
							queueMessage("Manual stand rejected: " + msg);
						} else {
							queueMessage("Manual stand assignment failed with unknown action: " + action);
						}
					}
				}
				catch (const std::exception& e) {
					queueMessage("Failed to parse stand assignment response: " + std::string(e.what()));
				}
			}
			break;
		}
	}
}

void RampAgent::OnTimer(int Counter) {
	isConnected_ = isConnected();
	isController_ = isController();
	
	{
		std::lock_guard<std::mutex> lock(messageQueueMutex_);
		for (const auto& msg : messageQueue_) {
			DisplayMessage(msg, "");
		}
		messageQueue_.clear();
	}

	if (Counter % 15 == 0) this->runUpdate();
}

std::string RampAgent::toUpper(std::string str)
{
	std::string result = str;
	std::transform(result.begin(), result.end(), result.begin(), ::toupper);
	return result;
}

std::pair<bool, CRadarTarget> rampAgent::RampAgent::aircraftExists(const std::string& callsign)
{
	CRadarTarget target = RadarTargetSelectFirst();
	while (target.IsValid()) {
		if (toUpper(target.GetCallsign()) == toUpper(callsign)) {
			return { true, target };
		}
		target = RadarTargetSelectNext(target);
	}
	return { false, CRadarTarget() };
}

std::vector<std::pair<CRadarTarget, CFlightPlan>> RampAgent::getAllAircraftsAndFP()
{
	std::vector<std::pair<CRadarTarget, CFlightPlan>> result;
	CRadarTarget target = myPluginInstance->RadarTargetSelectFirst();
	while (target.IsValid()) {
		std::pair<CRadarTarget, CFlightPlan> pair;
		pair.first = target;
		pair.second = target.GetCorrelatedFlightPlan();
		result.push_back(pair);
		target = myPluginInstance->RadarTargetSelectNext(target);
	}
	return result;
}

void RampAgent::getAllAssignedStands()
{
	nlohmann::ordered_json response;

	// Safely copy callsign and apiUrl
	std::string localCallsign;
	std::string localApiUrl;
	{
		std::lock_guard<std::mutex> lock(callsignMutex_);
		localCallsign = callsign_;
	}
	{
		std::lock_guard<std::mutex> lock(apiUrlMutex_);
		localApiUrl = apiUrl_;
	}

	httplib::SSLClient cli(localApiUrl, 443);
	cli.set_connection_timeout(0, 700000); // 700ms
	cli.set_read_timeout(1, 0);            // 1s
	cli.set_write_timeout(1, 0);           // 1s
	httplib::Headers headers = { {"User-Agent", "EuroscopeRampAgent"} };

	auto res = cli.Get("/api/occupancy/?callsign=" + localCallsign, headers);

	if (res && res->status >= 200 && res->status < 300) {
		if (!printError.exchange(true)) { // reset error printing flag on success 
			queueMessage("Successfully reconnected to Ramp Agent server.");
		}
		try {
			if (!res->body.empty()) response = nlohmann::ordered_json::parse(res->body);
			std::lock_guard<std::mutex> lock(assignedStandsMutex_);
			assignedStands_ = response;
			return;
		}
		catch (const std::exception& e) {
			queueMessage("Failed to parse occupied stands data from Ramp Agent server: " + std::string(e.what()));
			std::lock_guard<std::mutex> lock(assignedStandsMutex_);
			assignedStands_ = nlohmann::ordered_json::object();
			return;
		}
	}
	else {
		if (printError.exchange(false)) {
			if (firstTime.load() == false) {
				queueMessage("Failed to retrieve occupied stands data from Ramp Agent server. HTTP status: " + std::to_string(res ? res->status : 0));
			}
			else {
				firstTime.store(false);
			}
		}
	}

	std::lock_guard<std::mutex> lock(assignedStandsMutex_);
	assignedStands_ = nlohmann::ordered_json::object();
}

CFlightPlanControllerAssignedData rampAgent::RampAgent::getControllerAssignedData(const std::string callsign)
{
	CFlightPlan fp = FlightPlanSelectFirst();
	while (fp.IsValid()) {
		if (toUpper(fp.GetCallsign()) == toUpper(callsign)) {
			return fp.GetControllerAssignedData();
		}
		fp = FlightPlanSelectNext(fp);
	}
	return CFlightPlanControllerAssignedData();
}

bool rampAgent::RampAgent::isConnected()
{
	bool userIsConnected = this->GetConnectionType() != EuroScopePlugIn::CONNECTION_TYPE_NO;
	return userIsConnected;
}

bool rampAgent::RampAgent::isController()
{
	bool userIsObserver = std::string_view(this->ControllerMyself().GetCallsign()).ends_with("_OBS") == true || this->ControllerMyself().GetFacility() == 0;
	
	{
		std::lock_guard<std::mutex> lock(callsignMutex_);
		callsign_ = std::string(this->ControllerMyself().GetCallsign());
	}
	
	return !userIsObserver;
}

void rampAgent::RampAgent::sortStandList(std::vector<std::string>& standList)
{
	std::sort(standList.begin(), standList.end(), [](const std::string& a, const std::string& b) {
		auto key = [](const std::string& s) {
			size_t i = 0, n = s.size();

			// Trim leading spaces
			while (i < n && std::isspace(static_cast<unsigned char>(s[i]))) ++i;

			// Leading number
			int num = 0;
			bool hasNum = false;
			while (i < n && std::isdigit(static_cast<unsigned char>(s[i]))) {
				hasNum = true;
				int digit = s[i] - '0';
				if (num > ((std::numeric_limits<int>::max)() - digit) / 10)
					num = (std::numeric_limits<int>::max)(); // clamp overflow
				else
					num = num * 10 + digit;
				++i;
			}

			// Immediate letter suffix (A, B, AB, ...)
			std::string letters;
			while (i < n && std::isalpha(static_cast<unsigned char>(s[i]))) {
				letters.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(s[i]))));
				++i;
			}

			// Remainder (case-insensitive)
			std::string tailUpper;
			tailUpper.reserve(n - i);
			for (; i < n; ++i) {
				unsigned char c = static_cast<unsigned char>(s[i]);
				tailUpper.push_back(static_cast<char>(std::toupper(c)));
			}

			// Bare names (no numeric prefix) go to the end
			return std::tuple<int, std::string, std::string, std::string>(
				hasNum ? num : (std::numeric_limits<int>::max)(), letters, tailUpper, s
			);
			};

		const auto [an, al, ar, as] = key(a);
		const auto [bn, bl, br, bs] = key(b);

		if (an != bn) return an < bn;

		// If numbers equal, empty suffix (e.g., "2") comes before "2A"
		if (al != bl) {
			if (al.empty() != bl.empty()) return al.empty();
			return al < bl;
		}

		// Fallback: remainder, then original
		if (ar != br) return ar < br;
		return as < bs;
		});
}

inline std::string rampAgent::RampAgent::generateToken(const std::string& callsign)
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

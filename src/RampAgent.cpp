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
	//cli->enable_server_certificate_verification(false);

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

			Stand standInfo{ .name = standName, .icao = icao, .remark = remark, .occupied = true };

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

void RampAgent::SendStandAssignementRequest(httplib::SSLClient& cli, const std::string& userCallsign, const std::string& callsign, const Stand& standInfo)
{
	//TODO: implement
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
}

bool RampAgent::IsConnected()
{
	bool userIsConnected = this->GetConnectionType() == EuroScopePlugIn::CONNECTION_TYPE_DIRECT;
	return userIsConnected;
}

bool RampAgent::IsController()
{
	const std::string callsign = this->ControllerMyself().GetCallsign();
	if (callsign.size() < 3) return false;

	bool userIsObserver =  callsign.substr(callsign.size() - 3) == "OBS" || this->ControllerMyself().GetFacility() == 0;
	
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

//void RampAgent::processApiRequest(httplib::SSLClient& cli, const ApiRequest& request) {
//	httplib::Headers headers = { {"User-Agent", "EuroscopeRampAgent"} };
//
//	try {
//		switch (request.type) {
//		case ApiRequestType::FETCH_OCCUPANCY:
//		{
//			// This is handled by getAllAssignedStands() which is already implemented
//			break;
//		}
//
//		case ApiRequestType::FETCH_STANDS:
//		{
//			std::string apiEndpoint = "/api/airports/" + request.icao + "/stands";
//			auto res = cli.Get(apiEndpoint.c_str(), headers);
//
//			if (res && res->status >= 200 && res->status < 300) {
//				try {
//					if (!res->body.empty()) {
//						nlohmann::ordered_json standsJson = nlohmann::ordered_json::parse(res->body);
//
//						// Cache the stands data
//						{
//							std::lock_guard<std::mutex> lock(standsDataCacheMutex_);
//							standsDataCache_ = standsJson;
//						}
//
//						if (!printError.load(std::memory_order_relaxed)) {
//							printError.store(true, std::memory_order_relaxed);
//							queueMessage("Successfully retrieved stands information for " + request.icao);
//						}
//					}
//				}
//				catch (const nlohmann::json::exception& e) {
//					queueMessage("Failed to parse stands data: " + std::string(e.what()));
//				}
//				catch (const std::exception& e) {
//					queueMessage("Failed to parse stands data: " + std::string(e.what()));
//				}
//			}
//			else {
//				if (printError.load(std::memory_order_relaxed)) {
//					printError.store(false, std::memory_order_relaxed);
//					queueMessage("Failed to get stands information. HTTP status: " + std::to_string(res ? res->status : 0));
//				}
//			}
//			break;
//		}
//
//		case ApiRequestType::ASSIGN_STAND:
//		{
//			std::string localCallsign;
//			{
//				std::lock_guard<std::mutex> lock(callsignMutex_);
//				localCallsign = callsign_;
//			}
//
//			std::string token = generateToken(localCallsign);
//			std::string apiEndpoint = "/api/assign?stand=" + request.standName +
//				"&icao=" + request.icao +
//				"&callsign=" + request.callsign +
//				"&token=" + token +
//				"&client=" + localCallsign;
//
//			auto res = cli.Get(apiEndpoint.c_str(), headers);
//
//			if (!res || !(res->status >= 200 && res->status < 300)) {
//				queueMessage("Failed to send manual assign. HTTP status: " + std::to_string(res ? res->status : 0));
//				return;
//			}
//
//			if (!res->body.empty()) {
//				try {
//					nlohmann::ordered_json dataJson = nlohmann::ordered_json::parse(res->body);
//
//					if (!dataJson.contains("message")) {
//						queueMessage("Malformed response from server");
//						return;
//					}
//
//					auto& message = dataJson["message"];
//					if (!message.contains("action") || !message["action"].is_string()) {
//						queueMessage("Malformed response from server");
//						return;
//					}
//
//					std::string action = message["action"].get<std::string>();
//
//					if (action == "assign") {
//						{
//							std::lock_guard<std::mutex> lock(lastStandTagMapMutex_);
//							lastStandTagMap_[request.callsign] = request.standName;
//						}
//						{
//							std::lock_guard<std::mutex> lock(manualAssignedCallsignsMutex_);
//							manualAssignedCallsigns_[request.callsign] = request.standName;
//						}
//						UpdateTagItems(request.callsign, WHITE, request.standName);
//						queueMessage("Stand " + request.standName + " assigned to " + request.callsign);
//					}
//					else if (action == "free") {
//						{
//							std::lock_guard<std::mutex> lock(lastStandTagMapMutex_);
//							lastStandTagMap_.erase(request.callsign);
//						}
//						{
//							std::lock_guard<std::mutex> lock(manualAssignedCallsignsMutex_);
//							manualAssignedCallsigns_[request.callsign] = "";
//						}
//						UpdateTagItems(request.callsign, WHITE, "");
//						queueMessage("Stand freed for " + request.callsign);
//					}
//					else {
//						if (message.contains("message") && message["message"].is_string()) {
//							std::string msg = message["message"].get<std::string>();
//							queueMessage("Manual stand rejected: " + msg);
//						}
//						else {
//							queueMessage("Manual stand assignment failed with unknown action: " + action);
//						}
//					}
//				}
//				catch (const nlohmann::json::exception& e) {
//					queueMessage("Failed to parse stand assignment response: " + std::string(e.what()));
//				}
//				catch (const std::exception& e) {
//					queueMessage("Failed to parse stand assignment response: " + std::string(e.what()));
//				}
//			}
//			break;
//		}
//		}
//	}
//	catch (const std::exception& e) {
//		queueMessage("Exception in processApiRequest: " + std::string(e.what()));
//	}
//}
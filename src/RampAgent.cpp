// Exactly one translation unit defines the shim (INTEGRATION.md A2): it carries file
// static attach state, so a second definition would mean a second, separately attached
// copy. Must come before anything else pulls in esbridge.h, since the shim block sits
// inside the header's own include guard.
#define ESB_CLIENT_SHIM
#include <esbridge.h>

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
		// Queued, not displayed: Initialize runs from the constructor, so Euroscope has
		// not yet been handed the instance and cannot be called back into. OnTimer drains
		// this once we are registered.
		QueueError("Failed to initialize Ramp Agent: " + std::string(e.what()));
	}
}

void RampAgent::Shutdown()
{
	if (initialized_)
	{
		initialized_ = false;
	}
	
	// Signal worker thread to stop with proper memory ordering. Written under m_stopMutex
	// so the worker cannot test the predicate and then start waiting past the notify.
	{
		std::lock_guard<std::mutex> lock(m_stopMutex);
		m_stop.store(true, std::memory_order_release);
	}
	m_stopCv.notify_all();

	// Wait for worker thread to finish
	if (m_thread.joinable())
		m_thread.join();

	// Mandatory (A10). A provider left registered after its DLL unloads is exactly the
	// dangling-module case the bridge's reaping exists to catch, and relying on being
	// caught is not a plan. No subscriptions to release: this plugin only publishes.
	if (esb_api != nullptr && bridgeProvider_ != nullptr) {
		esb_api->unregister_provider(bridgeProvider_);
		bridgeProvider_ = nullptr;
	}

	// Deliberately silent. Shutdown only runs from ~RampAgent, itself reached from
	// EuroScopePlugInExit, so DisplayUserMessage here calls into Euroscope while it is
	// unregistering us - a well known crash on unload. Nobody reads a shutdown notice in
	// a closing chat window anyway.
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

	// Retried until it succeeds. A network hiccup while Euroscope was starting used to
	// leave airportStandsCache_ empty for the whole session: every stand menu answered
	// "not supported" and the only way back was restarting Euroscope.
	bool standMapReady = PopulateICAOStandMap(*cli);
	std::chrono::seconds populateBackoff{ 5 };
	auto nextPopulateAttempt = std::chrono::steady_clock::now() + populateBackoff;

	while (m_stop.load(std::memory_order_acquire) == false) {
		static size_t counter = 0;

		if (standMapReady == false && std::chrono::steady_clock::now() >= nextPopulateAttempt) {
			standMapReady = PopulateICAOStandMap(*cli);
			if (standMapReady == false) {
				// Backs off to a 5 minute ceiling, so a long outage costs little
				populateBackoff = std::min(populateBackoff * 2, std::chrono::seconds{ 300 });
				nextPopulateAttempt = std::chrono::steady_clock::now() + populateBackoff;
			}
		}

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

		// Avoid busy waiting, but stay interruptible: an unrestartable 100ms sleep meant
		// Shutdown had to wait it out on top of any request already in flight.
		std::unique_lock<std::mutex> lock(m_stopMutex);
		m_stopCv.wait_for(lock, std::chrono::milliseconds(100),
			[this] { return m_stop.load(std::memory_order_acquire); });
	}
}

void RampAgent::FetchAndUpdateAssignedStands(httplib::SSLClient& cli, const std::string& userCallsign)
{
	httplib::Headers headers = { {"User-Agent", "EuroscopeRampAgent"} };

	// Percent-encoded rather than concatenated: the callsign comes from Euroscope, and
	// anything reserved in it would otherwise change the shape of the query.
	const std::string apiEndpoint = "/api/occupancy/?callsign=" +
		httplib::encode_query_component(userCallsign, false);
	auto res = cli.Get(apiEndpoint, headers);

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

bool RampAgent::ReportStandMapFailure(const std::string& reason)
{
	// One message per outage: not one per failed airport, and not one per retry. Cleared
	// again on success, so a later outage is still reported.
	if (standMapErrorReported_ == false) {
		standMapErrorReported_ = true;
		QueueError("Could not load stand data (" + reason + "). Retrying in the background.");
	}
	return false;
}

bool rampAgent::RampAgent::PopulateICAOStandMap(httplib::SSLClient& cli)
{
	httplib::Headers headers = { {"User-Agent", "EuroscopeRampAgent"} };
	auto res = cli.Get("/api/airports/", headers);

	nlohmann::ordered_json response;

	if (res && res->status >= 200 && res->status < 300) {
		try {
			if (!res->body.empty()) response = nlohmann::ordered_json::parse(res->body);
			else return ReportStandMapFailure("empty compatible airports response");
		}
		catch (const nlohmann::json::exception& e) {
			return ReportStandMapFailure("unparseable compatible airports: " + std::string(e.what()));
		}
		catch (const std::exception& e) {
			return ReportStandMapFailure("unparseable compatible airports: " + std::string(e.what()));
		}
	}
	else {
		return ReportStandMapFailure("compatible airports request failed, HTTP status " +
			std::to_string(res ? res->status : 0) +
			(res ? "" : ", " + httplib::to_string(res.error())));
	}

	// Parse response to get list of compatible airports
	std::vector<std::string> compatibleAirports;
	try {
		if (!response.is_array()) return ReportStandMapFailure("compatible airports response is not an array");

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
		return ReportStandMapFailure("could not process compatible airports: " + std::string(e.what()));
	}
	catch (const std::exception& e) {
		return ReportStandMapFailure("could not process compatible airports: " + std::string(e.what()));
	}


	// For each compatible airport, fetch the stands and populate airportStandsCache_.
	// Failures are counted rather than reported individually: this is retried, so one
	// message per airport per attempt would bury the chat window.
	size_t unavailable = 0;

	for (const auto& icao : compatibleAirports) {
		// These run sequentially, each up to 2s connect + 1s read. Against a slow or
		// unreachable API that is minutes of work, and Shutdown joins this thread - so
		// give up as soon as we are asked to, rather than blocking Euroscope's unload.
		if (m_stop.load(std::memory_order_acquire)) return false;

		// Skip what a previous attempt already fetched, so a retry only chases the gaps
		{
			std::lock_guard<std::mutex> lock(standsCacheMutex_);
			if (airportStandsCache_.contains(icao)) continue;
		}

		// Same reasoning as the query parameters, but this one lands in the path, so a
		// stray '/' or '?' in an airport name would restructure the request rather than
		// just corrupt one value. Only the ICAO is encoded; the surrounding separators
		// are ours and must stay literal.
		const std::string standsEndpoint = "/api/airports/" +
			httplib::encode_path_component(icao) + "/stands";
		auto standsRes = cli.Get(standsEndpoint, headers);

		if (standsRes && standsRes->status >= 200 && standsRes->status < 300 && !standsRes->body.empty()) {
			try {
				nlohmann::ordered_json standsJson = nlohmann::ordered_json::parse(standsRes->body);
				std::vector<Stand> stands;
				for (const auto& [standName, standInfo] : standsJson.items()) {
					if (!standInfo.is_object()) continue;
					stands.push_back(Stand{ .name = standName, .icao = icao, .remark = "" });
				}
				std::lock_guard<std::mutex> lock(standsCacheMutex_);
				airportStandsCache_[icao] = std::move(stands);
			}
			catch (const std::exception&) {
				++unavailable; // Retried on the next attempt
			}
		}
		else {
			++unavailable;
		}
	}

	if (unavailable != 0) {
		return ReportStandMapFailure(std::to_string(unavailable) + " of " +
			std::to_string(compatibleAirports.size()) + " airports unavailable");
	}

	// Only announced when it follows a failure, so a clean start stays quiet
	if (standMapErrorReported_) {
		standMapErrorReported_ = false;
		QueueMessage("Stand data loaded for " + std::to_string(compatibleAirports.size()) + " airports.");
	}

	return true;
}

void RampAgent::SendStandAssignementRequest(httplib::SSLClient& cli, const std::string& userCallsign, const std::string& callsign, const Stand& standInfo)
{
	httplib::Headers headers = { {"User-Agent", "EuroscopeRampAgent"} };
	
	std::string token = GenerateToken(userCallsign);

	// Percent-encoded rather than concatenated: stand names come from the API and
	// callsigns from Euroscope, so a space, '&', '#' or '%' in either would truncate the
	// request or inject an extra query parameter. space_as_plus is off, so a space
	// becomes %20 - decoded by any server - rather than '+', which only form-style query
	// parsers translate back.
	const auto encode = [](const std::string& value) {
		return httplib::encode_query_component(value, false);
	};

	const std::string apiEndpoint = "/api/assign?stand=" + encode(standInfo.name) +
		"&icao=" + encode(standInfo.icao) +
		"&callsign=" + encode(callsign) +
		"&token=" + encode(token) +
		"&client=" + encode(userCallsign);

	auto res = cli.Get(apiEndpoint, headers);
	
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

	// Publish stand data over the bridge (done here to be independant from tag items, and
	// because the bridge ABI may only be called from the main thread)
	PublishStandsToBridge();
}

bool RampAgent::IsConnected()
{
	bool userIsConnected = this->GetConnectionType() == EuroScopePlugIn::CONNECTION_TYPE_DIRECT;
	return userIsConnected;
}

bool RampAgent::RegisterBridgeProvider(const ESB_Api_v1* api)
{
	// The doc strings are what ".esb schema rampagent" prints, and in practice the only
	// documentation a consumer will read (B1.3).
	static const ESB_FieldDecl fields[] = {
		{ BRIDGE_STAND_FIELD, ESB_T_STR, ESB_SCOPE_AIRCRAFT, 0, BRIDGE_STAND_MAX_BYTES,
		  "Stand assigned by the Ramp Agent service, empty when none is held" },
		{ BRIDGE_REMARK_FIELD, ESB_T_STR, ESB_SCOPE_AIRCRAFT, 0, BRIDGE_REMARK_MAX_BYTES,
		  "Free text remark attached to that stand assignment" },
	};

	ESB_ProviderDecl decl = {};
	decl.struct_size = sizeof decl;
	decl.provider_id = BRIDGE_PROVIDER_ID;
	decl.schema_major = 1;
	decl.schema_minor = 0;
	decl.display_name = "Ramp Agent";
	decl.contact = "https://github.com/AlexisBalzano/EuroscopeRampAgent";
	decl.fields = fields;
	decl.field_count = static_cast<uint32_t>(std::size(fields));
	decl.module = ESB_SelfModule(); // Never GetModuleHandleA by name: users rename DLLs (A9)

	const ESB_Status status = api->register_provider(&decl, &bridgeProvider_);
	if (status != ESB_OK) {
		bridgeProvider_ = nullptr;

		// A taken id is a conflict to settle with the other author, not a condition to
		// retry around (B1.6), so say it once and stop attempting.
		if (status == ESB_E_PROVIDER_TAKEN) {
			bridgeProviderConflict_ = true;
			DisplayError("Another loaded plugin already owns the \"" +
				std::string(BRIDGE_PROVIDER_ID) + "\" bridge provider id. Stand data will not be published.");
		}
		return false;
	}

	// Resolved once and cached; never called from the publish loop (B1.7)
	if (api->own_field(bridgeProvider_, BRIDGE_STAND_FIELD, &bridgeStandField_) != ESB_OK ||
		api->own_field(bridgeProvider_, BRIDGE_REMARK_FIELD, &bridgeRemarkField_) != ESB_OK) {
		api->unregister_provider(bridgeProvider_);
		bridgeProvider_ = nullptr;
		bridgeStandField_ = ESB_FIELD_NONE;
		bridgeRemarkField_ = ESB_FIELD_NONE;
		return false;
	}

	return true;
}

void RampAgent::PublishStandsToBridge()
{
	if (bridgeProviderConflict_) return;

	// Attached from OnTimer rather than the constructor (A4): Euroscope's plugin load
	// order follows the user's settings file, so the bridge may legitimately load after
	// us. Cheap once attached - a single pointer test.
	const ESB_Api_v1* api = ESB_Attach();
	if (api == nullptr) {
		// One message, once, using the shared wording so a user running several
		// bridge-aware plugins is told the same thing once rather than three ways (A7).
		if (++bridgeMissingTicks_ == BRIDGE_MISSING_TICKS_BEFORE_WARNING)
			DisplayError(ESB_MISSING_MESSAGE);
		return;
	}

	if (bridgeProvider_ == nullptr && RegisterBridgeProvider(api) == false)
		return;

	// The worker thread must never touch the bridge (A8), so the cache is snapshotted
	// here on the main thread and published from the copy. Same rule as the Euroscope
	// SDK: take the lock, copy, release, then call out.
	std::unordered_map<std::string, Stand> stands;
	{
		std::lock_guard<std::mutex> lock(standsCacheMutex_);
		stands = standsCache_;
	}

	// Swept over radar targets rather than over the cache, because an aircraft losing its
	// stand needs its published value cleared, and that aircraft is by definition absent
	// from the cache. No ground speed filter: that existed to spare Euroscope needless
	// assigned data writes, and an arrival's stand is worth showing before it lands.
	CRadarTarget rt = this->RadarTargetSelectFirst();
	while (rt.IsValid()) {
		const std::string callsign = SafeString(rt.GetCallsign());
		if (callsign.empty()) {
			rt = this->RadarTargetSelectNext(rt);
			continue;
		}

		// Resolved from the callsign every sweep rather than cached, so a reconnection
		// can never leave us writing through a stale handle (B2.9).
		ESB_Aircraft ac = ESB_AIRCRAFT_NONE;
		if (api->aircraft(callsign.c_str(), &ac) != ESB_OK) {
			rt = this->RadarTargetSelectNext(rt); // Bridge has not seen this one yet
			continue;
		}

		// No dirty tracking on purpose (B1.9): the bridge compares before storing, so
		// republishing an unchanged value costs nothing and notifies nobody.
		if (const auto stand = stands.find(callsign); stand != stands.end()) {
			// Clamped to the declared caps: the bridge rejects an over-long write outright
			// rather than trimming it, which would leave the consumer with nothing at all.
			// ESB_Str borrows the buffer, so these locals must outlive the set_ac call.
			const std::string name = stand->second.name.substr(0, BRIDGE_STAND_MAX_BYTES);
			const std::string remark = stand->second.remark.substr(0, BRIDGE_REMARK_MAX_BYTES);

			ESB_Value nameValue = ESB_Str(name.c_str());
			api->set_ac(bridgeProvider_, ac, bridgeStandField_, &nameValue);

			ESB_Value remarkValue = ESB_Str(remark.c_str());
			api->set_ac(bridgeProvider_, ac, bridgeRemarkField_, &remarkValue);
		}
		else {
			api->clear_ac(bridgeProvider_, ac, bridgeStandField_);
			api->clear_ac(bridgeProvider_, ac, bridgeRemarkField_);
		}

		rt = this->RadarTargetSelectNext(rt);
	}
}

void RampAgent::ClearBridgeStand(const std::string& callsign)
{
	if (esb_api == nullptr || bridgeProvider_ == nullptr || callsign.empty()) return;

	ESB_Aircraft ac = ESB_AIRCRAFT_NONE;
	if (esb_api->aircraft(callsign.c_str(), &ac) != ESB_OK) return;

	esb_api->clear_ac(bridgeProvider_, ac, bridgeStandField_);
	esb_api->clear_ac(bridgeProvider_, ac, bridgeRemarkField_);
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
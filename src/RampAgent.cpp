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
	// Writing controller assigned data needs an active controller connection. Without one
	// SetFlightStripAnnotation fails and leaves the annotation untouched, so the "value
	// differs" guard below stays true and every on ground aircraft is retried every tick.
	if (isConnected_.load(std::memory_order_acquire) == false ||
		isController_.load(std::memory_order_acquire) == false) {
		lastAnnotationWrite_.clear(); // Start clean on the next connection
		return;
	}

	// Snapshot the cache and release the lock before calling into Euroscope. OnGetTagItem
	// takes this same non-recursive mutex on this same thread, so holding it across SDK
	// calls turns any re-entry from Euroscope into a self deadlock.
	std::unordered_map<std::string, Stand> stands;
	{
		std::lock_guard<std::mutex> lock(standsCacheMutex_);
		stands = standsCache_;
	}

	std::unordered_map<std::string, AnnotationWrite> attempted;

	CRadarTarget rt = this->RadarTargetSelectFirst();
	while (rt.IsValid()) {
		const std::string callsign = SafeString(rt.GetCallsign());

		// Only update strip if target is on the ground
		if (callsign.empty() || rt.GetGS() > ON_GROUND_SPEED_THRESHOLD) {
			rt = this->RadarTargetSelectNext(rt);
			continue;
		}

		CFlightPlan fp = rt.GetCorrelatedFlightPlan();
		if (fp.IsValid() == false) {
			rt = this->RadarTargetSelectNext(rt);
			continue;
		}

		// Empty values clear the annotation when no stand information is available
		AnnotationWrite desired;
		if (const auto stand = stands.find(callsign); stand != stands.end()) {
			desired.stand = stand->second.name.substr(0, MAX_ANNOTATION_LENGTH);
			desired.remark = stand->second.remark.substr(0, MAX_ANNOTATION_LENGTH);
		}

		// Euroscope refused these exact values last tick and nothing has changed since,
		// so asking again would only repeat the same rejection.
		if (const auto previous = lastAnnotationWrite_.find(callsign);
			previous != lastAnnotationWrite_.end() && previous->second.rejected &&
			previous->second.stand == desired.stand && previous->second.remark == desired.remark) {
			attempted[callsign] = previous->second;
			rt = this->RadarTargetSelectNext(rt);
			continue;
		}

		CFlightPlanControllerAssignedData assignedData = fp.GetControllerAssignedData();

		// Writes only on a difference, so an annotation cleared by anything else is still
		// restored, but re-reads afterwards rather than trusting the return value:
		// Euroscope reports success even when it stores less than it was handed, and a
		// silently truncated value differs again next tick and would be rewritten on
		// every single one. Treating that as a rejection lets the caller suppress it.
		const auto writeAnnotation = [&assignedData](int index, const std::string& value) {
			if (SafeString(assignedData.GetFlightStripAnnotation(index)) == value)
				return true; // Already correct, nothing to do

			if (assignedData.SetFlightStripAnnotation(index, value.c_str()) == false)
				return false;

			return SafeString(assignedData.GetFlightStripAnnotation(index)) == value;
		};

		bool accepted = writeAnnotation(STAND_FLIGHT_STRIP_INDEX, desired.stand);
		accepted &= writeAnnotation(REMARK_FLIGHT_STRIP_INDEX, desired.remark);

		desired.rejected = !accepted;
		attempted[callsign] = desired;

		rt = this->RadarTargetSelectNext(rt);
	}

	lastAnnotationWrite_ = std::move(attempted);
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
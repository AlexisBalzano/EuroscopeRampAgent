#include <numeric>
#include <algorithm>
#include <limits>
#include <cctype>
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
#ifndef DEV
	std::pair<bool, std::string> updateAvailable = newVersionAvailable();
	if (updateAvailable.first) {
		DisplayMessage("A new version of Ramp Agent is available: " + updateAvailable.second + " (current version: " + RAMPAGENT_VERSION + ")", "");
	}
#endif // !DEV

	try
	{
		initialized_ = true;
		RegisterTagItems();
		RegisterTagActions();
	}
	catch (const std::exception& e)
	{
		DisplayMessage("Failed to initialize Ramp Agent: " + std::string(e.what()), "Error");
	}
	m_stop = false;
	DisplayMessage("Ramp Agent initialized successfully", "Status");
	DisplayMessage("Remember to use .rampAgent connect once connected to network.", "");
}

std::pair<bool, std::string> rampAgent::RampAgent::newVersionAvailable()
{
	httplib::SSLClient cli("api.github.com");
	cli.set_connection_timeout(0, 700000); // 700ms
	cli.set_read_timeout(1, 0);            // 1s
	cli.set_write_timeout(1, 0);           // 1s
	cli.set_keep_alive(false);             // don't hold sockets open
	httplib::Headers headers = { {"User-Agent", "EuroscopeRampAgentVersionChecker"} };
	std::string apiEndpoint = "/repos/AlexisBalzano/EuroscopeRampAgent/releases/latest";

	auto res = cli.Get(apiEndpoint.c_str(), headers);
	if (res && res->status == 200) {
		try
		{
			auto json = nlohmann::json::parse(res->body);
			std::string latestVersion = json["tag_name"];
			if (latestVersion != RAMPAGENT_VERSION) {
				DisplayMessage("A new version of Ramp Agent is available: " + latestVersion + " (current version: " + RAMPAGENT_VERSION + ")");
				return { true, latestVersion };
			}
			else {
				return { false, "" };
			}
		}
		catch (const std::exception& e)
		{
			DisplayMessage("Failed to parse version information from GitHub: " + std::string(e.what()));
			return { false, "" };
		}
	}
	else {
		DisplayMessage("Failed to check for Ramp Agent updates. HTTP status: " + std::to_string(res ? res->status : 0));
		return { false, "" };
	}
}

void RampAgent::Shutdown()
{
	if (initialized_)
	{
		initialized_ = false;
	}
	m_stop = true;
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


	if (m_thread.joinable()) {
		m_thread.join();
	}
	m_thread = std::thread(&RampAgent::getAllAssignedStands, this);

	{
		std::lock_guard<std::mutex> lock(messageQueueMutex_);
		for (const auto& msg : messageQueue_) {
			DisplayMessage(msg, "");
		}
		messageQueue_.clear();
	}

	// AssignedStands_ is updated we can use it
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
		if (printError) {
			printError = false; // avoid spamming logs
			DisplayMessage("No occupied stands data received to update tags.", "");
		}
		// Clear All Tag Items
		for (const auto& [callsign, standName] : lastStandTagMapCopy) {
			UpdateTagItems(callsign, WHITE, "");
		}
		std::lock_guard<std::mutex> lock2(lastStandTagMapMutex_);
		lastStandTagMap_.clear();
		return;
	}

	std::unordered_map<std::string, std::string> standTagMap;

	auto& assigned = assignedStandsCopy["assignedStands"];
	assignedStandsCopy["assignedStands"].insert(
		assignedStandsCopy["assignedStands"].end(),
		assignedStandsCopy["occupiedStands"].begin(),
		assignedStandsCopy["occupiedStands"].end()
	); // display tag item on occupied stands as well

	for (auto& stand : assigned) {
		std::string callsign = stand["callsign"].get<std::string>();
		if (aircraftExists(callsign).first == false) {
			continue; // Aircraft not found, skip
		}

		std::string standName = stand["name"].get<std::string>();
		
		{
			std::lock_guard<std::mutex> lock(manualAssignedCallsignsMutex_);
			if (manualAssignedCallsigns_.find(callsign) != manualAssignedCallsigns_.end()) {
				standName = manualAssignedCallsigns_[callsign];
			}
		}

		standTagMap[callsign] = standName;

		std::string remark = stand.value("remark", "");

		// Update only if changed or new
		if (lastStandTagMapCopy.find(callsign) != lastStandTagMapCopy.end() && lastStandTagMapCopy[callsign] == standName) {
			UpdateTagItems(callsign, WHITE, standName, remark);
			continue;
		}
		else {
			UpdateTagItems(callsign, YELLOW, standName, remark);
		}
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

	std::lock_guard<std::mutex> lock2(lastStandTagMapMutex_);
	lastStandTagMap_ = standTagMap;
}

void RampAgent::OnTimer(int Counter) {
	if (Counter % 10 == 0) this->runUpdate();
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

	httplib::SSLClient cli(apiUrl_, 443);
	cli.set_connection_timeout(0, 700000); // 700ms
	cli.set_read_timeout(1, 0);            // 1s
	cli.set_write_timeout(1, 0);           // 1s
	httplib::Headers headers = { {"User-Agent", "EuroscopeRampAgent"} };

	auto res = cli.Get("/rampagent/api/occupancy/", headers);

	if (res && res->status >= 200 && res->status < 300) {
		if (!printError) {
			printError = true; // reset error printing flag on success
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
		if (printError) {
			printError = false; // avoid spamming logs
			queueMessage("Failed to retrieve occupied stands data from Ramp Agent server. HTTP status: " + std::to_string(res ? res->status : 0));
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
	if (!userIsConnected) DisplayMessage("Not connected to network.", "Status");
	return userIsConnected;
}

bool rampAgent::RampAgent::isController()
{
	bool userIsObserver = std::string_view(this->ControllerMyself().GetCallsign()).ends_with("_OBS") == true || this->ControllerMyself().GetFacility() == 0;
	callsign_ = std::string(this->ControllerMyself().GetCallsign());
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
	std::string s = AUTH_SECRET + callsign_;
	unsigned char hash[SHA256_DIGEST_LENGTH];
	SHA256(reinterpret_cast<const unsigned char*>(s.data()), s.size(), hash);
	std::ostringstream oss;
	oss << std::hex << std::setfill('0');
	for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
		oss << std::setw(2) << static_cast<int>(hash[i]);
	}
	return oss.str();
}

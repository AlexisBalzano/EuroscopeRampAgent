#include <numeric>
#include <algorithm>
#include <limits>
#include <cctype>
#include <httplib.h>
#include <fstream>
#include <filesystem>

#include "RampAgent.h"
#include "version.h"
#include "core/TagItem.h"
#include "core/CompileCommands.h"
#include "core/TagFunctions.h"

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
	}
	catch (const std::exception& e)
	{
		DisplayMessage("Failed to initialize Ramp Agent: " + std::string(e.what()), "Error");
	}
	m_stop = false;
	DisplayMessage("Ramp Agent initialized successfully", "Status");
	DisplayMessage("Remember to use .rampAgent connect once connected to network", "");
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
		if (printError) {
			printError = false; // avoid spamming logs
			DisplayMessage("Not connected to FSD server. Cannot generate report.", "");
		}
		return;
	}

	generateReport(lastReportJson_); // generate the report using euroscopeSDK so synchronous with euroscope data

	if (m_thread.joinable()) {
		m_thread.join();
	}
	if (canSendReport_) {
		m_thread = std::thread(&RampAgent::sendReport, this); // Will send report asynchronously and generate assignedStands_ when done
	}
	else {
		m_thread = std::thread(&RampAgent::getAllAssignedStands, this); // Will only update assignedStands_ without sending report
	}

	{
		std::lock_guard<std::mutex> lock(messageQueueMutex_);
		for (const auto& msg : messageQueue_) {
			DisplayMessage(msg, "");
		}
		messageQueue_.clear();
	}

	// AssignedStands_ is updated we can use it
	std::lock_guard<std::mutex> lock(assignedStandsMutex_);

	if (assignedStands_.empty()) {
		if (printError) {
			printError = false; // avoid spamming logs
			DisplayMessage("No occupied stands data received to update tags.", "");
		}
		// Clear All Tag Items
		for (const auto& [callsign, standName] : lastStandTagMap_) {
			//UpdateTagItems(callsign, WHITE, ""); //FIXME:
		}
		lastStandTagMap_.clear();
		return;
	}

	//updateStandMenuButtons(menuICAO_, assignedStands);

	std::map<std::string, std::string> standTagMap;

	auto& assigned = assignedStands_["assignedStands"];
	assignedStands_["assignedStands"].insert(
		assignedStands_["assignedStands"].end(),
		assignedStands_["occupiedStands"].begin(),
		assignedStands_["occupiedStands"].end()
	); // display tag item on occupied stands as well

	for (auto& stand : assigned) {
		std::string callsign = stand["callsign"].get<std::string>();
		if (aircraftExists(callsign) == false) {
			continue; // Aircraft not found, skip
		}

		std::string standName = stand["name"].get<std::string>();
		standTagMap[callsign] = standName;

		std::string remark = stand.value("remark", "");

		// Update only if changed or new
		if (lastStandTagMap_.find(callsign) != lastStandTagMap_.end() && lastStandTagMap_[callsign] == standName) {
			//UpdateTagItems(callsign, WHITE, standName, remark);
			continue;
		}
		else {
			//UpdateTagItems(callsign, YELLOW, standName, remark);
		}
	}

	// Clear tags for aircraft that are no longer assigned
	for (const auto& [callsign, standName] : lastStandTagMap_) {
		if (standTagMap.find(callsign) == standTagMap.end()) {
			//UpdateTagItems(callsign, WHITE, "");
		}
	}

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

bool rampAgent::RampAgent::aircraftExists(const std::string& callsign)
{
	CRadarTarget target = RadarTargetSelectFirst();
	while (target.IsValid()) {
		if (toUpper(target.GetCallsign()) == toUpper(callsign)) {
			return true;
		}
		target = RadarTargetSelectNext(target);
	}
	return false;
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

void RampAgent::generateReport(nlohmann::ordered_json& reportJson)
{
	std::lock_guard<std::mutex> lock(lastReportJsonMutex_);
	reportJson.clear();

	// need to retrieve all aircraft in range and format json report
	// Can make es crash since not used in the same block ?
	std::vector<std::pair<CRadarTarget, CFlightPlan>> acFPs = getAllAircraftsAndFP();

	// Filter for ground aircraft & Airborn aircrafts
	std::vector<std::pair<CRadarTarget, CFlightPlan>> groundAircrafts;
	std::vector<std::pair<CRadarTarget, CFlightPlan>> airbornAircrafts;
	for (const auto& ac : acFPs) {
		if (ac.first.GetGS() == 0) {
			groundAircrafts.push_back(ac);
		}
		else {
			if (ac.first.GetPosition().GetPressureAltitude() > 20000) continue; // Skip aircraft above 20,000 ft
			airbornAircrafts.push_back(ac);
		}
	}


	reportJson["client"] = callsign_;
	reportJson["aircrafts"]["onGround"] = nlohmann::ordered_json::object();
	reportJson["aircrafts"]["airborne"] = nlohmann::ordered_json::object();

	for (const auto& ac : groundAircrafts) {
		std::string callsign = toUpper(ac.first.GetCallsign());
		std::string origin = "N/A";
		std::string aircraftType = "ZZZZ";
		if (ac.second.IsValid()) {
			CFlightPlanData fp = ac.second.GetFlightPlanData();
			origin = toUpper(fp.GetOrigin());
			aircraftType = toUpper(fp.GetAircraftFPType());
		}

		reportJson["aircrafts"]["onGround"][callsign]["origin"] = origin;
		reportJson["aircrafts"]["onGround"][callsign]["aircraftType"] = aircraftType;
		reportJson["aircrafts"]["onGround"][callsign]["position"]["lat"] = ac.first.GetPosition().GetPosition().m_Latitude;
		reportJson["aircrafts"]["onGround"][callsign]["position"]["lon"] = ac.first.GetPosition().GetPosition().m_Longitude;
	}

	for (const auto& ac : airbornAircrafts) {
		std::string callsign = toUpper(ac.first.GetCallsign());
		if (!ac.second.IsValid()) continue; // Skip if no flightplan found
		CFlightPlanData fp = ac.second.GetFlightPlanData();
		std::string dest = std::string(fp.GetDestination());
		if (std::string(dest).substr(0, 2) != "LF") continue; // Skip if destination is not in France
		std::string origin = "N/A";
		std::string destination = "N/A";
		std::string aircraftType = "ZZZZ";
		origin = toUpper(fp.GetOrigin());
		destination = toUpper(dest);
		aircraftType = toUpper(fp.GetAircraftFPType());

		reportJson["aircrafts"]["airborne"][callsign]["origin"] = origin;
		reportJson["aircrafts"]["airborne"][callsign]["destination"] = destination;
		reportJson["aircrafts"]["airborne"][callsign]["aircraftType"] = aircraftType;
		reportJson["aircrafts"]["airborne"][callsign]["position"]["lat"] =ac.first.GetPosition().GetPosition().m_Latitude;
		reportJson["aircrafts"]["airborne"][callsign]["position"]["lon"] =ac.first.GetPosition().GetPosition().m_Longitude;
		reportJson["aircrafts"]["airborne"][callsign]["position"]["alt"] = ac.first.GetPosition().GetPressureAltitude();
		reportJson["aircrafts"]["airborne"][callsign]["position"]["dist"] = ac.second.GetDistanceToDestination();
	}
}

void RampAgent::sendReport()
{
	nlohmann::ordered_json reportJson;
	{
		std::lock_guard<std::mutex> lock(lastReportJsonMutex_);
		reportJson = lastReportJson_;
	}

	if (reportJson.empty()) {
		queueMessage("Skipping report: no data to send.");
		std::lock_guard<std::mutex> lock(assignedStandsMutex_);
		assignedStands_ = nlohmann::ordered_json::object();
		return;
	}

	httplib::SSLClient cli(apiUrl_, 443);
	cli.set_connection_timeout(0, 700000); // 700ms
	cli.set_read_timeout(1, 0);            // 1s
	cli.set_write_timeout(1, 0);           // 1s
	httplib::Headers headers = { {"User-Agent", "EuroscopeRampAgent"} };


	auto res = cli.Post("/api/report", headers, reportJson.dump(), "application/json");

	if (res && res->status >= 200 && res->status < 300) {
		printError = true; // reset error printing flag on success
		try {
			std::lock_guard<std::mutex> lock(assignedStandsMutex_);
			assignedStands_ = res->body.empty() ? nlohmann::ordered_json::object() : nlohmann::ordered_json::parse(res->body);
			return;
		}
		catch (const std::exception& e) {
			queueMessage("Failed to parse response from Ramp Agent server: " + std::string(e.what()));
			std::lock_guard<std::mutex> lock(assignedStandsMutex_);
			assignedStands_ = nlohmann::ordered_json::object();
			return;
		}
	}
	else {
		if (printError) {
			printError = false; // avoid spamming logs
			queueMessage("Failed to send report to Ramp Agent server. HTTP status: " + std::to_string(res ? res->status : 0));
		}
		std::lock_guard<std::mutex> lock(assignedStandsMutex_);
		assignedStands_ = nlohmann::ordered_json::object();
		return;
	}
	
	queueMessage("Unknown error");
	{
		std::lock_guard<std::mutex> lock(assignedStandsMutex_);
		assignedStands_ = nlohmann::ordered_json::object();
	}
}

void RampAgent::getAllAssignedStands()
{
	nlohmann::ordered_json response;

	httplib::SSLClient cli(apiUrl_, 443);
	cli.set_connection_timeout(0, 700000); // 700ms
	cli.set_read_timeout(1, 0);            // 1s
	cli.set_write_timeout(1, 0);           // 1s
	httplib::Headers headers = { {"User-Agent", "EuroscopeRampAgent"} };

	auto res = cli.Get("/api/occupancy/assigned", headers);

	if (res && res->status >= 200 && res->status < 300) {
		printError = true; // reset error printing flag on success
		try {
			if (!res->body.empty()) response["assignedStands"] = nlohmann::ordered_json::parse(res->body);
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

bool RampAgent::printToFile(const std::vector<std::string>& lines, const std::string& fileName)
{
	char path[MAX_PATH + 1] = { 0 };
	GetModuleFileNameA((HINSTANCE)&__ImageBase, path, MAX_PATH);
	std::filesystem::path dllPath = std::filesystem::path(path).parent_path();


	std::filesystem::path dir = dllPath / "logs" / "RampAgent";
	std::error_code ec;
	if (!std::filesystem::exists(dir))
	{
		if (!std::filesystem::create_directories(dir, ec))
		{
			DisplayMessage("Failed to create log directory: " + dir.string() + " ec=" + ec.message(), "");
			return false;
		}
	}
	std::filesystem::path filePath = dir / fileName;
	std::ofstream outFile(filePath);
	if (!outFile.is_open()) {
		DisplayMessage("Could not open file to write: " + filePath.string());
		return false;
	}
	for (const auto& line : lines) {
		outFile << line << std::endl;
	}
	outFile.close();
	return true;
}

bool rampAgent::RampAgent::dumpReportToLogFile()
{
	std::string fileName = "report_" + std::to_string(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())) + ".txt";
	std::vector<std::string> content;
	content.push_back("--- Ramp Agent last Report Dump ---");
	content.push_back("Is Connected: " + std::string(isConnected_ ? "Yes" : "No"));
	content.push_back("Can send report: " + std::string(canSendReport_ ? "Yes" : "No"));
	content.push_back("Report:");
	content.push_back(lastReportJson_.dump(4).empty() ? "{}" : lastReportJson_.dump(4));
	return printToFile(content, fileName);
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

void rampAgent::RampAgent::sortStandList(std::vector<Stand>& standList)
{
	std::sort(standList.begin(), standList.end(), [](const Stand& a, const Stand& b) {
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
				if (num > ((std::numeric_limits<int>::max)() - digit) / 10) {
					num = (std::numeric_limits<int>::max)(); // clamp on overflow
				}
				else {
					num = num * 10 + digit;
				}
				++i;
			}

			// Immediate letter suffix (A, B, AB, ...)
			std::string letters;
			while (i < n && std::isalpha(static_cast<unsigned char>(s[i]))) {
				letters.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(s[i]))));
				++i;
			}

			// Remainder (for stable tie-breaking, case-insensitive)
			std::string tailUpper;
			tailUpper.reserve(n - i);
			for (; i < n; ++i) {
				unsigned char c = static_cast<unsigned char>(s[i]);
				tailUpper.push_back(static_cast<char>(std::toupper(c)));
			}

			// Bare names (no numeric prefix) go to the end.
			return std::tuple<int, std::string, std::string, std::string>(
				hasNum ? num : (std::numeric_limits<int>::max)(), letters, tailUpper, s
			);
			};

		const auto [an, al, ar, as] = key(a.name);
		const auto [bn, bl, br, bs] = key(b.name);

		if (an != bn) return an < bn;

		// If numbers equal, empty suffix (e.g., "2") comes before non-empty (e.g., "2A")
		if (al != bl) {
			if (al.empty() != bl.empty()) return al.empty();
			return al < bl; // case-insensitive compare via uppercased letters
		}

		// Fallback to case-insensitive remainder, then original (stable) name
		if (ar != br) return ar < br;
		return as < bs;
		});
}

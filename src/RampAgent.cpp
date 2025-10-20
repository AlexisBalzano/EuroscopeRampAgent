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
		DisplayMessage("A new version of NeoRampAgent is available: " + updateAvailable.second + " (current version: " + RAMPAGENT_VERSION + ")", "");
	}
#endif // !DEV

	try
	{
		initialized_ = true;
		RegisterTagItems();

		isConnected_ = isConnected();
		canSendReport_ = isController();
	}
	catch (const std::exception& e)
	{
		DisplayMessage("Failed to initialize Ramp Agent: " + std::string(e.what()), "Error");
	}
	m_stop = false;
	m_thread = std::thread(&RampAgent::run, this);
	DisplayMessage("Ramp Agent initialized successfully", "Status");
}

std::pair<bool, std::string> rampAgent::RampAgent::newVersionAvailable()
{
	httplib::SSLClient cli("api.github.com");
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
		DisplayMessage("Failed to check for NeoRampAgent updates. HTTP status: " + std::to_string(res ? res->status : 0));
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

void RampAgent::runUpdate() {
	// Do stuff
}

void RampAgent::OnTimer(int Counter) {
	if (Counter % 10 == 0)
		this->runUpdate();
}

std::string rampAgent::RampAgent::toUpper(std::string str)
{
	std::string result = str;
	std::transform(result.begin(), result.end(), result.begin(), ::toupper);
	return result;
}

void rampAgent::RampAgent::generateReport(nlohmann::ordered_json& reportJson)
{
	////FIXME:
	//reportJson.clear();

	//// need to retrieve all aircraft in range and format json report
	//std::vector<Aircraft::Aircraft> aircrafts = aircraftAPI_->getAll();

	//// Filter for ground aircraft
	//std::vector<Aircraft::Aircraft> groundAircrafts;
	//for (const auto& ac : aircrafts) {
	//	if (ac.position.onGround && ac.position.groundSpeed == 0) {
	//		groundAircrafts.push_back(ac);
	//	}
	//}

	//// Filter for airborn aircraft
	//std::vector<Aircraft::Aircraft> airbornAircrafts;
	//for (const auto& ac : aircrafts) {
	//	if (!ac.position.onGround || ac.position.groundSpeed != 0) {
	//		if (ac.position.altitude > 20000) continue; // Skip aircraft above 20,000 ft
	//		airbornAircrafts.push_back(ac);
	//	}
	//}

	//if (!isConnected_) {
	//	if (printError) {
	//		printError = false; // avoid spamming logs
	//		DisplayMessage("Not connected to FSD server. Cannot send report.", "NeoRampAgent");
	//		logger_->log(Logger::LogLevel::Warning, "Not connected to FSD server. Cannot send report.");
	//	}
	//	return;
	//}

	//reportJson["client"] = callsign_;
	//reportJson["aircrafts"]["onGround"] = nlohmann::ordered_json::object();
	//reportJson["aircrafts"]["airborne"] = nlohmann::ordered_json::object();

	//for (const auto& ac : groundAircrafts) {
	//	std::string callsign = toUpper(ac.callsign);
	//	std::optional<Flightplan::Flightplan> fp = flightplanAPI_->getByCallsign(ac.callsign);
	//	std::string origin = "N/A";
	//	std::string aircraftType = "ZZZZ";
	//	if (fp.has_value()) {
	//		origin = toUpper(fp->origin);
	//		aircraftType = toUpper(fp->acType);
	//	}

	//	reportJson["aircrafts"]["onGround"][callsign]["origin"] = origin;
	//	reportJson["aircrafts"]["onGround"][callsign]["aircraftType"] = aircraftType;
	//	reportJson["aircrafts"]["onGround"][callsign]["position"]["lat"] = ac.position.latitude;
	//	reportJson["aircrafts"]["onGround"][callsign]["position"]["lon"] = ac.position.longitude;
	//}

	//for (const auto& ac : airbornAircrafts) {
	//	std::string callsign = toUpper(ac.callsign);
	//	std::optional<Flightplan::Flightplan> fp = flightplanAPI_->getByCallsign(ac.callsign);
	//	if (!fp.has_value()) continue; // Skip if no flightplan found
	//	if (fp->destination.substr(0, 2) != "LF") continue; // Skip if destination is not in France
	//	std::string origin = "N/A";
	//	std::string destination = "N/A";
	//	std::string aircraftType = "ZZZZ";
	//	if (fp.has_value()) {
	//		origin = toUpper(fp->origin);
	//		destination = toUpper(fp->destination);
	//		aircraftType = toUpper(fp->acType);
	//	}

	//	std::optional<double> distOpt = aircraftAPI_->getDistanceToDestination(ac.callsign);
	//	double dist = distOpt.value_or(-1);

	//	reportJson["aircrafts"]["airborne"][callsign]["origin"] = origin;
	//	reportJson["aircrafts"]["airborne"][callsign]["destination"] = destination;
	//	reportJson["aircrafts"]["airborne"][callsign]["aircraftType"] = aircraftType;
	//	reportJson["aircrafts"]["airborne"][callsign]["position"]["lat"] = ac.position.latitude;
	//	reportJson["aircrafts"]["airborne"][callsign]["position"]["lon"] = ac.position.longitude;
	//	reportJson["aircrafts"]["airborne"][callsign]["position"]["alt"] = ac.position.altitude;
	//	reportJson["aircrafts"]["airborne"][callsign]["position"]["dist"] = dist;
	//}
}

bool rampAgent::RampAgent::printToFile(const std::vector<std::string>& lines, const std::string& fileName)
{
	char path[MAX_PATH + 1] = { 0 };
	GetModuleFileNameA((HINSTANCE)&__ImageBase, path, MAX_PATH);
	std::filesystem::path dllPath = std::filesystem::path(path).parent_path();


	std::filesystem::path dir = dllPath / "logs" / "NeoRampAgent";
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
	content.push_back("--- NeoRampAgent last Report Dump ---");
	content.push_back("Is Connected: " + std::string(isConnected_ ? "Yes" : "No"));
	content.push_back("Can send report: " + std::string(canSendReport_ ? "Yes" : "No"));
	content.push_back("Report:");
	content.push_back(lastReportJson_.dump(4).empty() ? "{}" : lastReportJson_.dump(4));
	return printToFile(content, fileName);
}

void RampAgent::run() {
	int counter = 1;

	while (true) {
		counter += 1;
		std::this_thread::sleep_for(std::chrono::seconds(1));

		if (true == this->m_stop) {
			return;
		}

		this->OnTimer(counter);
	}
	return;
}

bool rampAgent::RampAgent::isConnected()
{
	int connectionType = GetConnectionType();
	if (connectionType == 1) return true;
	DisplayMessage("Not connected to an online network (Connection Type: " + std::to_string(connectionType) + ")", "Status");
	return false;
}

bool rampAgent::RampAgent::isController()
{
	CController myself = ControllerMyself();
	DisplayMessage("Myself is valid: " + std::string(myself.IsValid() ? "Yes" : "No"), "Status");
	if (myself.IsValid()) return myself.IsController();
	return false;
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

#include <numeric>
#include <httplib.h>

#include "RampAgent.h"
#include "version.h"

using namespace rampAgent;
using namespace EuroScopePlugIn;

RampAgent::RampAgent() : CPlugIn(EuroScopePlugIn::COMPATIBILITY_CODE, "RampAgent", PLUGIN_VERSION, "French vACC", "Open Source"), m_stop(false)
{
	Initialize();
};
RampAgent::~RampAgent()
{
	Shutdown();
};

RampAgent* myPluginInstance = nullptr;

void __declspec (dllexport) EuroScopePlugInInit(EuroScopePlugIn::CPlugIn** ppPlugInInstance)
{
	*ppPlugInInstance = myPluginInstance = new rampAgent::RampAgent();
}


void __declspec (dllexport) EuroScopePlugInExit()
{
	// delete the instance
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
		//TODO: Register tag items, commands, etc.

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
	int connectionType = myPluginInstance->GetConnectionType();
	if (connectionType == 1) return true;
	return false;
}

bool rampAgent::RampAgent::isController()
{
	CController myself = myPluginInstance->ControllerMyself();
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

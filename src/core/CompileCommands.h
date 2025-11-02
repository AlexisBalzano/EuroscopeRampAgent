#pragma once
#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include "RampAgent.h"

namespace rampAgent {

inline bool RampAgent::OnCompileCommand(const char* sCommandLine)
{
	if (sCommandLine == nullptr) return false;

	std::string line(sCommandLine);

	auto trim = [](std::string& s) {
		const auto notSpace = [](int ch) { return !std::isspace(ch); };
		s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
		s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
	};
	trim(line);
	if (line.empty()) return false;

	// Remove optional leading '.' (Euroscope command convention)
	if (!line.empty() && line[0] == '.') line.erase(0, 1);

	std::istringstream iss(line);
	std::string cmd;
	iss >> cmd;
	if (cmd.empty()) return false;

	auto toLower = [](std::string s) {
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return s;
	};
	const std::string lcmd = toLower(cmd);

	if (lcmd != "rampagent")
		return false;

	std::string sub;
	iss >> sub;
	sub = toLower(sub);

	if (sub == "version")
	{
		DisplayMessage(std::string("RampAgent version: ") + RAMPAGENT_VERSION, "");
		return true;
	}
	if (sub == "url")
	{
		std::string url;
		iss >> url;
		if (url.empty())
		{
			DisplayMessage("Usage: .ramp url <domain (no https://)>", "");
			return false;
		}
		changeApiUrl(url);
		DisplayMessage("API URL set to " + url, "");
		return true;
	}
	if (sub == "disconnect")
	{
		isConnected_ = false;
		isController_ = false;
		callsign_.clear();
		DisplayMessage("Disconnected.");
		return true;
	}
	DisplayMessage("Commands: .rampAgent version / .rampAgent disconnect / .rampAgent url <url>", "");
	return true;
}

} // namespace rampAgent
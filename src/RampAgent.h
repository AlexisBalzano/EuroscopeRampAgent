#pragma once
#include <Windows.h>
#include <EuroScopePlugIn.h>
#include <thread>
#include <string>
#include <nlohmann/json.hpp>

using namespace EuroScopePlugIn;

namespace rampAgent {

	class RampAgent;

	extern RampAgent* myPluginInstance;

	constexpr const char* RAMPAGENT_VERSION = "v0.0.1";

	struct Stand {
		std::string name;
		std::string icao;
		bool occupied;
	};


	class RampAgent : public CPlugIn
	{
	public:
		RampAgent();
		~RampAgent();

	public:
		// Plugin lifecycle methods
		void Initialize();
		std::pair<bool, std::string> newVersionAvailable();
		void Shutdown();
		void Reset();

		// Radar commands
		void DisplayMessage(const std::string& message, const std::string& sender = "");

		// Scope events
		void OnTimer(int Counter);
		// void OnFsdConnectionStateChange(const Fsd::FsdConnectionStateChangeEvent* event) override

		//std::string toUpper(std::string str);
		//void generateReport(nlohmann::ordered_json& reportJson);
		//nlohmann::ordered_json sendReport();
		//nlohmann::ordered_json getAllOccupiedStands(); //used to update tags when not sending reports
		//nlohmann::ordered_json getAllBlockedStands();
		//std::string getMenuICAO() const { return menuICAO_; }
		//std::string changeMenuICAO(const std::string& newICAO) { menuICAO_ = newICAO; return menuICAO_; }


	public:
		// Command IDs
		std::string versionId_;
		std::string menuId_;

	private:
		void runUpdate();
		void run();
		bool isConnected();
		bool isController();
		void sortStandList(std::vector<Stand>& standList);

	private:
		// Plugin state
		bool initialized_ = false;
		bool m_stop;
		std::thread m_thread;
		bool canSendReport_ = false;
		bool isConnected_ = false;
		std::string menuICAO_ = "LFPG"; //default airport for stand menu

		// Tag Items
		void RegisterTagItems();
		//void RegisterTagActions();
		//void RegisterCommand();
		//void unegisterCommand();
		//void OnTagAction(const Tag::TagActionEvent* event) override;
		//void OnTagDropdownAction(const Tag::DropdownActionEvent* event) override;
		//void UpdateTagItems();
		//void UpdateTagItems(std::string Callsign, std::string standName = "N/A");
		//void updateStandMenuButtons(const std::string& icao, const nlohmann::ordered_json& occupiedStands);

		// TAG Items IDs
		enum class TagItemID {
			TagItem_STAND = 0,
			TagItem_MENU
		};
	};
} // namespace rampAgent
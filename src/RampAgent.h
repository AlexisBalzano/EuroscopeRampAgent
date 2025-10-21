#pragma once
#include <Windows.h>
#include <EuroScopePlugIn.h>
#include <thread>
#include <string>
#include <nlohmann/json.hpp>
#include <mutex>

using namespace EuroScopePlugIn;

namespace rampAgent {

	class RampAgent;

	extern RampAgent* myPluginInstance;

	constexpr const char* RAMPAGENT_VERSION = "v0.0.1";
	constexpr const char* RAMPAGENT_API = ""; //FIXME: Add default API URL here

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
		void queueMessage(const std::string& message);

		// Scope events
		void OnTimer(int Counter) override;

		std::string toUpper(std::string str);
		bool aircraftExists(const std::string& callsign);
		std::vector<std::pair<CRadarTarget,CFlightPlan>> getAllAircraftsAndFP();
		void generateReport(nlohmann::ordered_json& reportJson);
		void sendReport();
		void getAllAssignedStands(); //used to update tags when not sending reports
		std::string getMenuICAO() const { return menuICAO_; }
		std::string changeMenuICAO(const std::string& newICAO) { menuICAO_ = newICAO; return menuICAO_; }
		bool printToFile(const std::vector<std::string>& lines, const std::string& fileName);
		bool dumpReportToLogFile();
		void changeApiUrl(const std::string& newUrl) { apiUrl_ = newUrl; }


	public:
		// Command IDs
		std::string versionId_;
		std::string menuId_;

	private:
		void runUpdate();
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
		bool printError = true;
		std::string menuICAO_ = "LFPG"; //default airport for stand menu
		std::map<std::string, std::string> lastStandTagMap_; // maps callsign to stand tag ID
		std::string apiUrl_ = RAMPAGENT_API;
		nlohmann::ordered_json lastReportJson_;
		std::mutex lastReportJsonMutex_;
		std::string callsign_;
		std::vector<std::string> messageQueue_;
		std::mutex messageQueueMutex_;
		nlohmann::ordered_json assignedStands_;
		std::mutex assignedStandsMutex_;


		// Tag Items
		void RegisterTagItems();
		void RegisterTagActions();
		bool OnCompileCommand(const char* sCommandLine);
		void UpdateTagItems(std::string Callsign, std::string standName = "", std::string remark = "");
		//void OnTagAction(const Tag::TagActionEvent* event) override;
		//void OnTagDropdownAction(const Tag::DropdownActionEvent* event) override;
		//void updateStandMenuButtons(const std::string& icao, const nlohmann::ordered_json& occupiedStands);

		// TAG Items IDs
		enum class TagItemID {
			TagItem_STAND = 0,
			TagItem_REMARK,
		};

		enum class TagActionID {
			TagAction_OpenMENU = 0,
		};
	};
} // namespace rampAgent
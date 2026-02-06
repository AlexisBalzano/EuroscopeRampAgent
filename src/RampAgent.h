#pragma once
#include <Windows.h>
#include <EuroScopePlugIn.h>
#include <thread>
#include <condition_variable>
#include <queue>
#include <string>
#include <nlohmann/json.hpp>
#include <mutex>

using namespace EuroScopePlugIn;

namespace rampAgent {

	class RampAgent;

	extern RampAgent* myPluginInstance;

	constexpr const char* RAMPAGENT_VERSION = "v1.0.7";
	constexpr const char* RAMPAGENT_API = "rampagent.vatsim.fr";

	COLORREF WHITE = RGB(255, 255, 255);
	COLORREF YELLOW = RGB(255, 220, 3);

	struct Stand {
		std::string name;
		std::string icao;
		bool occupied;
	};

	struct TagItemInfo {
		std::string standName;
		std::string remark;
		COLORREF color;
	};


	class RampAgent : public CPlugIn
	{
	public:
		RampAgent();
		~RampAgent();

	public:
		// Plugin lifecycle methods
		void Initialize();
		void Shutdown();
		void Reset();

		// Radar commands
		void DisplayMessage(const std::string& message, const std::string& sender = "");
		void queueMessage(const std::string& message);

		// Scope events
		void OnTimer(int Counter) override;

		std::string toUpper(std::string str);
		std::pair<bool, CRadarTarget> aircraftExists(const std::string& callsign);
		std::vector<std::pair<CRadarTarget,CFlightPlan>> getAllAircraftsAndFP();
		void getAllAssignedStands();
		CFlightPlanControllerAssignedData getControllerAssignedData(const std::string callsign);
		void changeApiUrl(const std::string& newUrl) { 
			std::lock_guard<std::mutex> lock(apiUrlMutex_);
			apiUrl_ = newUrl; 
		}
		std::string generateToken(const std::string& callsign);
		void assignStandToAircraft(const std::string& callsign, const std::string& standName, std::string menuIcao);

	public:
		// Command IDs
		std::string versionId_;

	private:
		// API request queue types (declared before methods that use them)
		enum class ApiRequestType {
			FETCH_OCCUPANCY,
			FETCH_STANDS,
			ASSIGN_STAND
		};
		
		struct ApiRequest {
			ApiRequestType type;
			std::string icao;
			std::string callsign;
			std::string standName;
		};

		void runUpdate();
		bool isConnected();
		bool isController();
		void sortStandList(std::vector<std::string>& standList);
		void workerThread(); // Background thread worker
		void processApiRequest(httplib::SSLClient& cli, const ApiRequest& request);
		void queueApiRequest(ApiRequestType type, const std::string& icao = "", const std::string& callsign = "", const std::string& standName = "");

	private:
		// Plugin state
		bool initialized_ = false;
		std::atomic<bool> m_stop{false};
		std::thread m_thread;
		std::condition_variable m_cv;
		std::mutex m_cvMutex;
		std::atomic<bool> m_fetchRequested{false};
		bool isController_ = false;
		bool isConnected_ = false;
		std::atomic<bool> printError{true};
		std::atomic<bool> firstTime{true};
		
		// Constants
		static constexpr size_t MAX_API_QUEUE_SIZE = 100;
		
		std::unordered_map<std::string, std::string> lastStandTagMap_; // used to determine if new value
		std::mutex lastStandTagMapMutex_;
		std::unordered_map<std::string, TagItemInfo> tagItemValueMap_; // maps callsign to stand tag ID
		std::mutex tagItemValueMapMutex_;
		std::string apiUrl_ = RAMPAGENT_API;
		std::mutex apiUrlMutex_;  // Protect apiUrl_
		std::string callsign_;
		std::mutex callsignMutex_;  // Protect callsign_
		std::vector<std::string> messageQueue_;
		std::mutex messageQueueMutex_;
		nlohmann::ordered_json assignedStands_;
		std::mutex assignedStandsMutex_;
		std::vector<std::string> menuButtons_;
		std::unordered_map<std::string, std::string> manualAssignedCallsigns_;
		std::mutex manualAssignedCallsignsMutex_;

		// API request queue
		std::queue<ApiRequest> apiRequestQueue_;
		std::mutex apiRequestQueueMutex_;
		
		// API response data
		nlohmann::ordered_json standsDataCache_; // Cache for stands data per ICAO
		std::mutex standsDataCacheMutex_;
		// Tag Items
		void RegisterTagItems();
		void RegisterTagActions();
		bool OnCompileCommand(const char* sCommandLine);
		void UpdateTagItems(std::string Callsign, COLORREF color = WHITE, std::string standName = "", std::string remark = ""); // Update tag items map
		void OnGetTagItem(EuroScopePlugIn::CFlightPlan FlightPlan, EuroScopePlugIn::CRadarTarget RadarTarget, int ItemCode,
			int TagData, char sItemString[16], int* pColorCode, COLORREF* pRGB, double* pFontSize) override; // Update euroscope Tag items
		void OnFunctionCall(int functionId, const char* itemString, POINT pt, RECT area) override;
		void updateStandMenuButtons(const std::string& icao);

		// TAG Items IDs
		enum class TagItemID {
			STAND = 0,
			REMARK,
		};

		enum class TagActionID {
			OpenMENU = 0,
			AssignSTAND,
		};
	};
} // namespace rampAgent
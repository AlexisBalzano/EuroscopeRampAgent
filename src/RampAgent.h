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

	struct Stand {
		std::string name;
		std::string icao;
		std::string remark;
	};

	struct TagItemInfo {
		std::string standName;
		std::string remark;
		COLORREF color;
	};

	enum TagItemID : int {
		STAND = 0,
		REMARK,
	};

	enum TagActionID : int {
		OpenMENU = 0,
		AssignStand,
	};


	class RampAgent : public EuroScopePlugIn::CPlugIn
	{
		static constexpr int PERIODIC_FETCH_TIME_INTERVAL = 10; // seconds
		static constexpr int ON_GROUND_SPEED_THRESHOLD = 70; // kts
		static constexpr int STAND_FLIGHT_STRIP_INDEX = 3;
		static constexpr int REMARK_FLIGHT_STRIP_INDEX = 4;
		static constexpr const char* API_URL = "rampagent.vatsim.fr";

	public:
		RampAgent();
		~RampAgent();

	public:
		// Plugin lifecycle methods
		void Initialize();
		void Shutdown();

		// Message management
		void DisplayMessage(const std::string& message);
		void QueueMessage(const std::string& message); // Needed since Euroscope is not threadsafe
		void DisplayError(const std::string& message);
		void QueueError(const std::string& message); // Needed since Euroscope is not threadsafe

		// Scope events
		void OnTimer(int Counter) override;
		void OnGetTagItem(EuroScopePlugIn::CFlightPlan FlightPlan, EuroScopePlugIn::CRadarTarget RadarTarget, int ItemCode,
						  int TagData, char sItemString[16], int* pColorCode, COLORREF* pRGB, double* pFontSize) override;
		void OnFunctionCall(int functionId, const char* itemString, POINT pt, RECT area) override;
		bool OnCompileCommand(const char* sCommandLine) override;

		// Tag item management
		void RegisterTagItems();
		void RegisterTagActions();


	private:
		bool IsController();
		bool IsConnected();
		void UpdateFlightStripAnnotations();

		void WorkerThread();
		void FetchAndUpdateAssignedStands(httplib::SSLClient& cli, const std::string& userCallsign);
		void PopulateICAOStandMap(httplib::SSLClient& cli);
		void SendStandAssignementRequest(httplib::SSLClient& cli, const std::string& userCallsign, const std::string& callsign, const Stand& standInfo);
		const std::string GenerateToken(const std::string& controllerCallsign);

	private:
		// Plugin state
		bool initialized_ = false;
		bool printError = true;
		std::atomic<bool> m_stop{false};
		std::thread m_thread;


		// Message management
		std::mutex messageQueueMutex_;
		std::vector<std::pair<std::string, bool>> messageQueue_; // Pair of message and isError flag

		// User state
		std::mutex userCallsignMutex_;
		std::atomic<bool> isController_ = false;
		std::atomic<bool> isConnected_ = false;
		std::string userCallsign_;

		// Stand data cache
		std::mutex standsCacheMutex_;
		std::unordered_map<std::string, Stand> standsCache_; // Callsign -> Stand info
		std::unordered_map<std::string, std::vector<Stand>> airportStandsCache_; // ICAO -> List of stands at the airport

		// API request management
		std::mutex apiRequestQueueMutex_;
		std::unordered_map<std::string, Stand> pendingAssignRequests_; // Callsign -> Stand info for pending assign requests
	};
} // namespace rampAgent
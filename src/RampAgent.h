#pragma once
#include <Windows.h>
#include <EuroScopePlugIn.h>
#include <thread>
#include <condition_variable>
#include <queue>
#include <string>
#include <nlohmann/json.hpp>
#include <mutex>
#include <esbridge.h>

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
		static constexpr const char* API_URL = "rampagent.vatsim.fr";

		// EuroScope Plugin Bridge. Stand data reaches vSMR through the bridge rather than
		// flight strip annotations 3/4, so nothing is laundered through Euroscope's
		// assigned data and nothing contends with UK Controller Plugin for index 3.
		// Values are published untruncated: the 15 character ceiling was a property of the
		// annotation slot, not of the data.
		static constexpr const char* BRIDGE_PROVIDER_ID = "rampagent";
		static constexpr const char* BRIDGE_STAND_FIELD = "stand";
		static constexpr const char* BRIDGE_REMARK_FIELD = "remark";
		// max_bytes is a hard cap the bridge enforces on write, not a truncation: an
		// over-long value would be rejected outright and the consumer would see nothing
		// at all. Set generously, and clamped again before publishing so an unexpectedly
		// long name degrades to a clipped one rather than disappearing.
		static constexpr uint32_t BRIDGE_STAND_MAX_BYTES = 32;
		static constexpr uint32_t BRIDGE_REMARK_MAX_BYTES = 128;
		static constexpr int BRIDGE_MISSING_TICKS_BEFORE_WARNING = 10; // INTEGRATION.md A7

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

		// Bridge publishing. Main thread only - the bridge ABI requires it (A8), so the
		// worker never touches any of this and the stand cache is snapshotted instead.
		void PublishStandsToBridge();
		bool RegisterBridgeProvider(const ESB_Api_v1* api);
		void ClearBridgeStand(const std::string& callsign);

		void WorkerThread();
		void FetchAndUpdateAssignedStands(httplib::SSLClient& cli, const std::string& userCallsign);
		bool PopulateICAOStandMap(httplib::SSLClient& cli); // True once every compatible airport is cached
		bool ReportStandMapFailure(const std::string& reason); // Always returns false, for use as a return value
		void SendStandAssignementRequest(httplib::SSLClient& cli, const std::string& userCallsign, const std::string& callsign, const Stand& standInfo);
		const std::string GenerateToken(const std::string& controllerCallsign);

	private:
		// Plugin state
		bool initialized_ = false;
		bool printError = true;
		bool standMapErrorReported_ = false; // Worker thread only; suppresses repeats while retrying
		std::atomic<bool> m_stop{false};
		// Lets Shutdown wake the worker out of its idle wait instead of sitting through
		// the remainder of it. m_stop is written under m_stopMutex so the worker cannot
		// evaluate the predicate and miss the notification in between.
		std::mutex m_stopMutex;
		std::condition_variable m_stopCv;
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

		// Bridge state. Main thread only, so no mutex. The provider handle is the write
		// authority; the field ids are resolved once and cached (B1.7).
		ESB_Provider* bridgeProvider_ = nullptr;
		ESB_FieldId bridgeStandField_ = ESB_FIELD_NONE;
		ESB_FieldId bridgeRemarkField_ = ESB_FIELD_NONE;
		int bridgeMissingTicks_ = 0;
		bool bridgeProviderConflict_ = false; // Another module owns "rampagent"; stop retrying

		// API request management
		std::mutex apiRequestQueueMutex_;
		std::unordered_map<std::string, Stand> pendingAssignRequests_; // Callsign -> Stand info for pending assign requests
	};
} // namespace rampAgent
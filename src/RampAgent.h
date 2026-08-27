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

	// What UpdateFlightStripAnnotations last pushed for a callsign. Euroscope rejecting a
	// write leaves the annotation unchanged, so without remembering the attempt the
	// "value differs" guard stays true and we retry it on every single tick.
	struct AnnotationWrite {
		std::string stand;
		std::string remark;
		bool rejected = false;
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
		// The SDK documents no maximum for SetFlightStripAnnotation and the previous 23
		// had no stated source. 15 is the SDK's own budget for plugin supplied tag text
		// (OnGetTagItem's char sItemString[16]) and is already what TagItem.h truncates
		// to, so both output paths now agree on one documented number. The write is also
		// verified in UpdateFlightStripAnnotations, so a shorter real limit gets detected
		// rather than assumed away.
		static constexpr size_t MAX_ANNOTATION_LENGTH = 15;
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

		// Flight strip annotation state. Touched only from OnTimer on the main thread, so
		// it needs no mutex. Rebuilt on every sweep, so departed aircraft drop out.
		std::unordered_map<std::string, AnnotationWrite> lastAnnotationWrite_; // Callsign -> last attempted write

		// API request management
		std::mutex apiRequestQueueMutex_;
		std::unordered_map<std::string, Stand> pendingAssignRequests_; // Callsign -> Stand info for pending assign requests
	};
} // namespace rampAgent
#pragma once
//TODO: Faire le tri
#include <memory>
#include <thread>
#include <vector>
#include <unordered_set>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <EuroScopePlugIn.h>

using namespace EuroScopePlugIn;

namespace rampAgent {
    class RampAgent : public CPlugIn
    {
    public:
        RampAgent();
        ~RampAgent();

		// Plugin lifecycle methods
		void Initialize();
		void Shutdown();
        void Reset();

        // Radar commands
        void DisplayMessage(const std::string& message, const std::string& sender = "");
		
        // Scope events
        void OnTimer(int Counter);


    private:
        void runUpdate();
        void run();

    private:
        // Plugin state
        bool initialized_ = false;
		bool m_stop;
		std::thread m_thread;

    };
} // namespace rampAgent
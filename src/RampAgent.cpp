#include "RampAgent.h"
#include "version.h"

using namespace rampAgent;

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
    try
    {
        initialized_ = true;
    }
    catch (const std::exception& e)
    {
		DisplayMessage("Failed to initialize Ramp Agent: " + std::string(e.what()), "Error");
    }
    m_stop = false;
    m_thread = std::thread(&RampAgent::run, this);
	DisplayMessage("Ramp Agent initialized successfully", "Status");
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

void RampAgent::DisplayMessage(const std::string &message, const std::string &sender) {
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
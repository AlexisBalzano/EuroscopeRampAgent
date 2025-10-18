#include "RampAgent.h"
//TODO: Faire le tri
#include <numeric>
#include <chrono>
#include <algorithm>

#include "Version.h"

using namespace rampAgent;

RampAgent::RampAgent() : CPlugIn(EuroScopePlugIn::COMPATIBILITY_CODE, "RampAgent", PLUGIN_VERSION, "French vACC", "Open Source"), m_stop(false)
{
    Initialize();
};
RampAgent::~RampAgent()
{
	Shutdown();
};

rampAgent::RampAgent* myPluginInstance = nullptr;

void __declspec (dllexport) EuroScopePlugInInit(EuroScopePlugIn::CPlugIn** ppPlugInInstance)
{
    // create the instance
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
		DisplayMessage("Failed to initialize EuroscopeRPC: " + std::string(e.what()), "Error");
    }
    m_stop = false;
    m_thread = std::thread(&EuroscopeRPC::run, this);
	DisplayMessage("EuroscopeRPC initialized successfully", "Status");
}

void EuroscopeRPC::Shutdown()
{
    if (initialized_)
    {
        initialized_ = false;
        trackedCallsigns_.clear();
    }
    m_stop = true;
    if (m_thread.joinable())
        m_thread.join();

	DisplayMessage("EuroscopeRPC shutdown complete", "Status");
}

void rpc::EuroscopeRPC::Reset()
{
}

void EuroscopeRPC::DisplayMessage(const std::string &message, const std::string &sender) {
    DisplayUserMessage("EuroscopeRPC", sender.c_str(), message.c_str(), true, true, false, false, false);
}

void rpc::EuroscopeRPC::discordSetup()
{
    discord::RPCManager::get()
        .setClientID(APPLICATION_ID)
        .onReady([this](discord::User const& user) {
		DisplayMessage("Connected to Discord as " + user.username + "#" + user.discriminator, "Discord");
            })
        .onDisconnected([this](int errcode, std::string_view message) {
		DisplayMessage("Disconnected from Discord: " + std::to_string(errcode) + " - " + std::string(message), "Discord");
            })
        .onErrored([this](int errcode, std::string_view message) {
		DisplayMessage("Discord error: " + std::to_string(errcode) + " - " + std::string(message), "Discord");
            });
}

void rpc::EuroscopeRPC::changeIdlingText()
{
    static int counter;
	counter++;
    static constexpr std::array<std::string_view, 21> idlingTexts = {
        "Waiting for traffic",
        "Monitoring frequencies",
        "Checking FL5000 for conflicts",
        "Watching the skies",
        "Searching for binoculars",
        "Listening to ATC chatter",
        "Scanning for aircraft",
        "Awaiting calls",
        "Tracking airspace",
        "Possible pilot deviation, I have a number...",
        "Clearing ILS 22R",
        "Observing traffic flow",
        "Monitoring silence",
        "Awaiting handoffs",
        "Recording ATIS",
        "Radar scope screensaver",
		"Checking NOTAMs",
		"Deleting SIDs from Flight Plans",
        "Answering radio check",
        "Trying to contact UNICOM",
        "Arguing that France is not on strike"
    };

    idlingText_ = std::string(idlingTexts[counter % idlingTexts.size()]);
}

void rpc::EuroscopeRPC::updatePresence()
{
    auto& rpc = discord::RPCManager::get();
    if (!m_presence) {
        rpc.clearPresence();
        return;
    }

    std::string controller = idlingText_;
	std::string state = "Idling";

    switch (connectionType_) {
    case State::CONTROLLING:
        controller = "Controlling " + currentController_ + " " + currentFrequency_;
        state = "Aircraft tracked: " + std::to_string(aircraftTracked_) + " of " + std::to_string(totalAircrafts_);
        rpc.getPresence().setSmallImageKey("radarlogo");
        break;
    case State::OBSERVING:
        controller = "Observing as " + currentController_;
        state = "Aircraft in range: " + std::to_string(totalAircrafts_);
        rpc.getPresence().setSmallImageKey("");
        break;
    case State::SWEATBOX:
        controller = "In Sweatbox";
        state = "Aircraft tracked: (" + std::to_string(aircraftTracked_) + " of " + std::to_string(totalAircrafts_) + ")";
        rpc.getPresence().setSmallImageKey("radarlogo");
        break;
    case State::PLAYBACK:
        controller = "In Playback";
        state = "Aircraft in range: " + std::to_string(totalAircrafts_);
        rpc.getPresence().setSmallImageKey("");
        break;
    default:
        rpc.getPresence().setSmallImageKey("");
        break;
    }

    std::string imageKey = "";
	std::string imageText = "";

    switch (tier_) {
        case Tier::SILVER:
            imageKey = "silver";
            imageText = "On a " + std::to_string(onlineTime_) + " hour streak";
            break;
        case Tier::GOLD:
            if (imageKey.empty()) imageKey = "gold";
            imageText = "On a " + std::to_string(onlineTime_) + " hour streak";
			break;
        case Tier::NONE:
        default:
            imageKey = "main";
            imageText = "French VACC";
			break;
    }

    if (isOnFire_) {
        imageKey += "fire";
        if (!imageText.empty()) imageText += " ";
        imageText += "On Fire!";
	}

    if (imageKey.empty()) imageKey = "main";
	if (imageText.empty()) imageText = "French VACC";


    rpc.getPresence()
        .setState(state)
		.setLargeImageKey(imageKey)
		.setLargeImageText(imageText)
        .setActivityType(discord::ActivityType::Game)
        .setStatusDisplayType(discord::StatusDisplayType::Name)
        .setDetails(controller)
        .setStartTimestamp(StartTime)
        .setSmallImageText("Total Tracks: " + std::to_string(totalTracks_))
        .setInstance(true)
        .refresh();
}

void rpc::EuroscopeRPC::updateData()
{
	updateConnectionType();
	getAicraftCount();

	if (std::time(nullptr) - StartTime > 2 * HOUR_THRESHOLD) tier_ = Tier::GOLD;
    else if (std::time(nullptr) - StartTime > HOUR_THRESHOLD) tier_ = Tier::SILVER;
	else tier_ = Tier::NONE;

	onlineTime_ = static_cast<int>((std::time(nullptr) - StartTime) / 3600); // in hours
    isOnFire_ = (aircraftTracked_ >= ONFIRE_THRESHOLD);
}

void rpc::EuroscopeRPC::updateConnectionType()
{
	connectionType_ = State::IDLE;
    CController selfController = myPluginInstance->ControllerMyself();
    int euroscopeConnectionType = myPluginInstance->GetConnectionType();
    switch (euroscopeConnectionType) {
    case CONNECTION_TYPE_NO:
        connectionType_ = State::IDLE;
        break;
    case CONNECTION_TYPE_DIRECT:
        if (selfController.IsController()) {
            connectionType_ = State::CONTROLLING;
            std::string freq = std::to_string(selfController.GetPrimaryFrequency());
            currentFrequency_ = freq.substr(0, freq.length() - 3);
        }
        else connectionType_ = State::OBSERVING;
        currentController_ = selfController.GetCallsign();
		std::transform(currentController_.begin(), currentController_.end(), currentController_.begin(), ::toupper);
        break;
    case CONNECTION_TYPE_SWEATBOX:
        connectionType_ = State::SWEATBOX;
        break;
    case CONNECTION_TYPE_PLAYBACK:
        connectionType_ = State::PLAYBACK;
        break;
    default:
        DisplayMessage("Unknown connection type: " + std::to_string(euroscopeConnectionType), "Error");
        connectionType_ = State::IDLE;
        break;
    }
}

void rpc::EuroscopeRPC::getAicraftCount()
{
	totalAircrafts_ = 0;
	aircraftTracked_ = 0;
    CRadarTarget target = myPluginInstance->RadarTargetSelectFirst();

    while (target.IsValid()) {
		++totalAircrafts_;
        if (target.GetCorrelatedFlightPlan().GetTrackingControllerIsMe()) {
            ++aircraftTracked_;
            if (trackedCallsigns_.insert(target.GetCallsign()).second) {
                ++totalTracks_;
            }
        }
        target = myPluginInstance->RadarTargetSelectNext(target);
	}
}

void EuroscopeRPC::runUpdate() {
	this->updatePresence();
}

void EuroscopeRPC::OnTimer(int Counter) {
    if (Counter % 5 == 0) // Every 5 seconds
        updateData();
    if (Counter % 15 == 0) // Every 15 seconds
        changeIdlingText();
    this->runUpdate();
}

void EuroscopeRPC::run() {
    int counter = 1;
    discordSetup();
    discord::RPCManager::get().initialize();
    auto& rpc = discord::RPCManager::get();

    while (true) {
        counter += 1;
        std::this_thread::sleep_for(std::chrono::seconds(1));

        if (true == this->m_stop) {
            discord::RPCManager::get().shutdown();
            return;
        }
        
        this->OnTimer(counter);
    }
    return;
}
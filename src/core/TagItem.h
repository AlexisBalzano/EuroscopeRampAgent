#pragma once
#include "RampAgent.h"
#include "Helpers.h"

using namespace rampAgent;

void RampAgent::RegisterTagItems() {
	RegisterTagItemType("STAND", static_cast<int>(TagItemID::STAND));
	RegisterTagItemType("REMARK", static_cast<int>(TagItemID::REMARK));
}

inline void RampAgent::OnGetTagItem(EuroScopePlugIn::CFlightPlan FlightPlan, EuroScopePlugIn::CRadarTarget RadarTarget, int ItemCode, int TagData, char sItemString[16], int* pColorCode, COLORREF* pRGB, double* pFontSize)
{
	std::lock_guard<std::mutex> lock(standsCacheMutex_);
	std::ignore = TagData;
	std::ignore = pRGB;
	std::ignore = pFontSize;

	*pColorCode = EuroScopePlugIn::TAG_COLOR_RGB_DEFINED;

	// Validate FlightPlan before accessing
	if (!FlightPlan.IsValid()) {
		return;
	}
	
	const char* callsignPtr = FlightPlan.GetCallsign();
	if (callsignPtr == nullptr || strlen(callsignPtr) == 0) {
		return; // Invalid callsign
	}
	
	std::string callsign = ToUpper(callsignPtr);

	if (!standsCache_.contains(callsign)) {
		return; // No tag info found for this callsign
	}

	bool shouldUpdateStrip = RadarTarget.IsValid() && RadarTarget.GetGS() <= ON_GROUND_SPEED_THRESHOLD; // Only update strip if target is on the ground

	try {
		switch (static_cast<TagItemID>(ItemCode)) {
			case TagItemID::STAND:
			{
				std::string standName = standsCache_[callsign].name;
				// Ensure we don't overflow the 16-byte buffer (15 chars + null terminator)
				size_t maxLen = std::min(standName.length(), size_t(15));
				std::snprintf(sItemString, 16, "%.*s", static_cast<int>(maxLen), standName.c_str());
				*pRGB = RGB(255, 255, 255);
				if (shouldUpdateStrip) {
					CFlightPlanControllerAssignedData assignedData = FlightPlan.GetControllerAssignedData();
					std::string truncatedStand = standName.length() > 23 ? standName.substr(0, 23) : standName; // Truncate to 23 characters to ensure it fits in the annotation field
					assignedData.SetFlightStripAnnotation(STAND_FLIGHT_STRIP_INDEX, truncatedStand.c_str());
				}
				break;
			}
			case TagItemID::REMARK:
			{
				std::string remark = standsCache_[callsign].remark;
				// Ensure we don't overflow the 16-byte buffer (15 chars + null terminator)
				size_t maxLen = std::min(remark.length(), size_t(15));
				std::snprintf(sItemString, 16, "%.*s", static_cast<int>(maxLen), remark.c_str());
				*pRGB = RGB(255, 255, 255);
				if (shouldUpdateStrip) {
					CFlightPlanControllerAssignedData assignedData = FlightPlan.GetControllerAssignedData();
					std::string truncatedRemark = remark.length() > 23 ? remark.substr(0, 23) : remark; // Truncate to 23 characters to ensure it fits in the annotation field
					assignedData.SetFlightStripAnnotation(REMARK_FLIGHT_STRIP_INDEX, truncatedRemark.c_str());
				}
				break;
			}
			default:
				break;
		}
	}
	catch (...) {
		// Silently fail - don't crash EuroScope
	}
}
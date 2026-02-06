#pragma once
#include "RampAgent.h"

using namespace rampAgent;

void RampAgent::RegisterTagItems() {
	RegisterTagItemType("STAND", static_cast<int>(TagItemID::STAND));
	RegisterTagItemType("REMARK", static_cast<int>(TagItemID::REMARK));
}

inline void RampAgent::UpdateTagItems(std::string callsign, COLORREF color, std::string standName, std::string remark)
{
	std::lock_guard<std::mutex> lock(tagItemValueMapMutex_);
	TagItemInfo tagInfo;
	tagInfo.standName = standName;
	tagInfo.remark = remark;
	tagInfo.color = color;

	tagItemValueMap_[callsign] = tagInfo;

	// Set scratchpad value for the stand to appear inside vSMR when aircraft is on ground (ie speed < 60kt)
	std::pair<bool, CRadarTarget> aircraft = aircraftExists(callsign);

	if (aircraft.first == false) return; // Aircraft not found, skip

	if (aircraft.second.GetGS() > 60) return; // Aircraft is not on ground, skip

	try {
		CFlightPlanControllerAssignedData assignedData = getControllerAssignedData(callsign);
		// Truncate to 23 characters to ensure it fits in the annotation field
		std::string truncatedStand = standName.length() > 23 ? standName.substr(0, 23) : standName;
		std::string truncatedRemark = remark.length() > 23 ? remark.substr(0, 23) : remark;
		
		assignedData.SetFlightStripAnnotation(3, truncatedStand.c_str());
		assignedData.SetFlightStripAnnotation(4, truncatedRemark.c_str());
	}
	catch (const std::exception& e) {
		// Silently fail - EuroScope API may throw if flight plan is invalid
	}
}

inline void RampAgent::OnGetTagItem(EuroScopePlugIn::CFlightPlan FlightPlan, EuroScopePlugIn::CRadarTarget RadarTarget, int ItemCode, int TagData, char sItemString[16], int* pColorCode, COLORREF* pRGB, double* pFontSize)
{
	std::lock_guard<std::mutex> lock(tagItemValueMapMutex_);
	std::ignore = RadarTarget;
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
	
	std::string callsign = toUpper(callsignPtr);

	if (tagItemValueMap_.find(callsign) == tagItemValueMap_.end()) {
		return; // No tag info found for this callsign
	}

	try {
		switch (static_cast<TagItemID>(ItemCode)) {
			case TagItemID::STAND:
			{
				std::string standName = tagItemValueMap_[callsign].standName;
				// Ensure we don't overflow the 16-byte buffer (15 chars + null terminator)
				size_t maxLen = std::min(standName.length(), size_t(15));
				std::snprintf(sItemString, 16, "%.*s", static_cast<int>(maxLen), standName.c_str());
				*pRGB = tagItemValueMap_[callsign].color;
				break;
			}
			case TagItemID::REMARK:
			{
				std::string remark = tagItemValueMap_[callsign].remark;
				// Ensure we don't overflow the 16-byte buffer (15 chars + null terminator)
				size_t maxLen = std::min(remark.length(), size_t(15));
				std::snprintf(sItemString, 16, "%.*s", static_cast<int>(maxLen), remark.c_str());
				*pRGB = tagItemValueMap_[callsign].color;
				break;
			}
			default:
				break;
		}
	}
	catch (const std::exception& e) {
		// Silently fail - don't crash EuroScope
	}
}
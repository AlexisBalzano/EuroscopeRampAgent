#pragma once
#include "RampAgent.h"

using namespace rampAgent;

void RampAgent::RegisterTagItems() {
	RegisterTagItemType("STAND", static_cast<int>(TagItemID::STAND));
	RegisterTagItemType("REMARK", static_cast<int>(TagItemID::REMARK));
}

inline void RampAgent::UpdateTagItems(std::string Callsign, COLORREF color, std::string standName, std::string remark)
{
	std::lock_guard<std::mutex> lock(tagItemValueMapMutex_);
	TagItemInfo tagInfo;
	tagInfo.standName = standName;
	tagInfo.remark = remark;
	tagInfo.color = color;

	tagItemValueMap_[Callsign] = tagInfo;
}

inline void RampAgent::OnGetTagItem(EuroScopePlugIn::CFlightPlan FlightPlan, EuroScopePlugIn::CRadarTarget RadarTarget, int ItemCode, int TagData, char sItemString[16], int* pColorCode, COLORREF* pRGB, double* pFontSize)
{
	std::lock_guard<std::mutex> lock(tagItemValueMapMutex_);
	std::ignore = RadarTarget;
	std::ignore = TagData;
	std::ignore = pRGB;
	std::ignore = pFontSize;

	*pColorCode = EuroScopePlugIn::TAG_COLOR_RGB_DEFINED;

	// FIXME: what happens if FlightPlan is not valid?
	std::string callsign = toUpper(FlightPlan.GetCallsign());

	if (tagItemValueMap_.find(callsign) == tagItemValueMap_.end()) {
		return; // No tag info found for this callsign
	}


	switch (static_cast<TagItemID>(ItemCode)) {
		case TagItemID::STAND:
		{
			std::string standName = tagItemValueMap_[callsign].standName;
			std::snprintf(sItemString, 16, "%s", standName.c_str());
			*pRGB = tagItemValueMap_[callsign].color;
			break;
		}
		case TagItemID::REMARK:
		{
			std::string remark = tagItemValueMap_[callsign].remark;
			std::snprintf(sItemString, 16, "%s", remark.c_str());
			*pRGB = tagItemValueMap_[callsign].color;
			break;
		}
		default:
			break;
	}
}
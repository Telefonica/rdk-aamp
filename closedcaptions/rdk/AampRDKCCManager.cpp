/*
 * If not stated otherwise in this file or this component's license file the
 * following copyright and licenses apply:
 *
 * Copyright 2018 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
*/

/**
 *  @file AampRDKCCManager.cpp
 *
 *  @brief Impl of RDK ClosedCaption integration layer
 *
 */

#include "AampRDKCCManager.h"

#include "priv_aamp.h" // Included for AAMPLOG
//TODO: Fix cyclic dependency btw GlobalConfig and AampLogManager

#include "AampJsonObject.h" // For JSON parsing
#include "AampUtils.h" // For aamp_StartsWith
#include "ccDataReader.h"
#include "vlCCConstants.h"
#include "cc_util.h"
#include "vlGfxScreen.h"

#define CHAR_CODE_1 49
#define CHAR_CODE_6 54

/**
 * @brief Singleton instance
 */
AampRDKCCManager *AampRDKCCManager::mInstance = NULL;

//Utility function to parse incoming style options

/**
 * @brief Get color option from input string
 *
 * @param[in] attributeIndex - CC attribute
 * @param[in] ccType - CC type 
 * @param[in] input - input color style
 * @param[out] colorOut - color option for the input value
 * @return int - 0 for success, -1 for failure
 */
static int getColor(gsw_CcAttribType attributeIndex, gsw_CcType ccType, std::string input, gsw_CcColor *colorOut)
{
	static gsw_CcColor* ccColorCaps[GSW_CC_COLOR_MAX];
	static bool flagForMalloc = false;
	static const int MAX_COLOR_BUFFER_LEN = ((GSW_MAX_CC_COLOR_NAME_LENGTH > 8 ? GSW_MAX_CC_COLOR_NAME_LENGTH : 8) + 1);
	unsigned int ccSizeOfCaps = 0;
	AAMPLOG_TRACE("%s:%d input: %s", __FUNCTION__, __LINE__, input.c_str());

	if (!input.empty() && colorOut)
	{
		if (!flagForMalloc)
		{
			flagForMalloc = true;
			for (int i = 0; i < GSW_CC_COLOR_MAX; i++)
			{
				ccColorCaps[i] = (gsw_CcColor*) malloc(sizeof(gsw_CcColor));
				memset(ccColorCaps[i], 0, sizeof(gsw_CcColor));
			}
		}

		ccGetCapability(attributeIndex, ccType, (void**) &ccColorCaps, &ccSizeOfCaps);
		bool found = false;
		const char *inputStr = input.c_str();
		for (unsigned int i = 0; i < ccSizeOfCaps; i++)
		{
			AAMPLOG_TRACE("%s:%d color caps: %s", __FUNCTION__, __LINE__, ccColorCaps[i]->name);
			if (0 == strncasecmp(ccColorCaps[i]->name, inputStr, GSW_MAX_CC_COLOR_NAME_LENGTH))
			{
				AAMPLOG_TRACE("%s:%d found match %s", __FUNCTION__, __LINE__, ccColorCaps[i]->name);
				memcpy(colorOut, ccColorCaps[i], sizeof (gsw_CcColor));
				found = true;
				break;
			}
		}
		if(!found)
		{
			AAMPLOG_ERR("%s:%d Unsupported color type %s", __FUNCTION__, inputStr);
		}
	}
	else
	{
		AAMPLOG_ERR("%s:%d Input is NULL!", __FUNCTION__, __LINE__);
		return -1;
	}
	return 0;
}

/**
 * @brief Get opacity value from input string
 *
 * @param[in] input - input opacity style
 * @param[out] opacityOut - opacity option for the input value
 * @return int - 0 for success, -1 for failure
 */
static int getOpacity(std::string input, gsw_CcOpacity *opacityOut)
{
	AAMPLOG_TRACE("%s:%d input: %s", __FUNCTION__, __LINE__, input.c_str());
	if (!input.empty() && opacityOut)
	{
		const char *inputStr = input.c_str();
		if (0 == strncasecmp(inputStr, "solid", strlen("solid")))
		{
			*opacityOut = GSW_CC_OPACITY_SOLID;
		}
		else if (0 == strncasecmp(inputStr, "flash", strlen("flash")))
		{
			*opacityOut = GSW_CC_OPACITY_FLASHING;
		}
		else if (0 == strncasecmp(inputStr, "translucent", strlen("translucent")))
		{
			*opacityOut = GSW_CC_OPACITY_TRANSLUCENT;
		}
		else if (0 == strncasecmp(inputStr, "transparent", strlen("transparent")))
		{
			*opacityOut = GSW_CC_OPACITY_TRANSPARENT;
		}
		else if (0 == strncasecmp(inputStr, "auto", strlen("auto")))
		{
			*opacityOut = GSW_CC_OPACITY_EMBEDDED;
		}
		else
		{
			AAMPLOG_ERR("%s:%d Unsupported opacity type %s", __FUNCTION__, __LINE__, inputStr);
		}
	}
	else
	{
		AAMPLOG_ERR("%s:%d Input is NULL", __FUNCTION__, __LINE__);
		return -1;
	}
	return 0;
}

/**
 * @brief Get font size value from input string
 *
 * @param[in] input - input font size value
 * @param[out] fontSizeOut - font size option for the input value
 * @return int - 0 for success, -1 for failure
 */
static int getFontSize(std::string input, gsw_CcFontSize *fontSizeOut)
{
	AAMPLOG_TRACE("%s:%d input: %s", __FUNCTION__, __LINE__, input.c_str());
	if (!input.empty() && fontSizeOut)
	{
		const char *inputStr = input.c_str();
		if (0 == strncasecmp(inputStr, "small", strlen("small")))
		{
			*fontSizeOut = GSW_CC_FONT_SIZE_SMALL;
		}
		else if ((0 == strncasecmp(inputStr, "standard", strlen("standard"))) ||
			(0 == strncasecmp(inputStr, "medium", strlen("medium"))))
		{
			*fontSizeOut = GSW_CC_FONT_SIZE_STANDARD;
		}
		else if (0 == strncasecmp(inputStr, "large", strlen("large")))
		{
			*fontSizeOut = GSW_CC_FONT_SIZE_LARGE;
		}
		else if (0 == strncasecmp(inputStr, "auto", strlen("auto")))
		{
			*fontSizeOut = GSW_CC_FONT_SIZE_EMBEDDED;
		}
		else
		{
			AAMPLOG_ERR("%s:%d Unsupported font size type %s", __FUNCTION__, __LINE__, inputStr);
		}
	}
	else
	{
		AAMPLOG_ERR("%s:%d Input is NULL", __FUNCTION__, __LINE__);
		return -1;
	}
	return 0;
}

/**
 * @brief Get font style value from input string
 *
 * @param[in] input - input font style value
 * @param[out] fontSizeOut - font style option for the input value
 * @return int - 0 for success, -1 for failure
 */
static int getFontStyle(std::string input, gsw_CcFontStyle *fontStyleOut)
{
	AAMPLOG_TRACE("%s:%d input: %s", __FUNCTION__, __LINE__, input.c_str());
	if (!input.empty() && fontStyleOut)
	{
		const char *inputStr = input.c_str();
		if (0 == strncasecmp(inputStr, "default", strlen("default")))
		{
			memcpy(fontStyleOut, GSW_CC_FONT_STYLE_DEFAULT, sizeof(gsw_CcFontStyle));
		}
		else if ((0 == strncasecmp(inputStr, "monospaced_serif", strlen("monospaced_serif"))) ||
			(0 == strncasecmp(inputStr, "Monospaced serif", strlen("Monospaced serif"))))
		{
			memcpy(fontStyleOut, GSW_CC_FONT_STYLE_MONOSPACED_SERIF, sizeof(gsw_CcFontStyle));
		}
		else if ((0 == strncasecmp(inputStr, "proportional_serif", strlen("proportional_serif"))) ||
			(0 == strncasecmp(inputStr, "Proportional serif", strlen("Proportional serif"))))
		{
			memcpy(fontStyleOut, GSW_CC_FONT_STYLE_PROPORTIONAL_SERIF, sizeof(gsw_CcFontStyle));
		}
		else if ((0 == strncasecmp(inputStr, "monospaced_sanserif", strlen("monospaced_sanserif"))) ||
			(0 == strncasecmp(inputStr, "Monospaced sans serif", strlen("Monospaced sans serif"))))
		{
			memcpy(fontStyleOut, GSW_CC_FONT_STYLE_MONOSPACED_SANSSERIF, sizeof(gsw_CcFontStyle));
		}
		else if ((0 == strncasecmp(inputStr, "proportional_sanserif", strlen("proportional_sanserif"))) ||
			(0 == strncasecmp(inputStr, "Proportional sans serif", strlen("Proportional sans serif"))))
		{
			memcpy(fontStyleOut, GSW_CC_FONT_STYLE_PROPORTIONAL_SANSSERIF, sizeof(gsw_CcFontStyle));
		}
		else if (0 == strncasecmp(inputStr, "casual", strlen("casual")))
		{
			memcpy(fontStyleOut, GSW_CC_FONT_STYLE_CASUAL, sizeof(gsw_CcFontStyle));
		}
		else if (0 == strncasecmp(inputStr, "cursive", strlen("cursive")))
		{
			memcpy(fontStyleOut, GSW_CC_FONT_STYLE_CURSIVE, sizeof(gsw_CcFontStyle));
		}
		else if ((0 == strncasecmp(inputStr, "smallcaps", strlen("smallcaps"))) ||
			(0 == strncasecmp(inputStr, "small capital", strlen("small capital"))))
		{
			memcpy(fontStyleOut, GSW_CC_FONT_STYLE_SMALL_CAPITALS, sizeof(gsw_CcFontStyle));
		}
		else if (0 == strncasecmp(inputStr, "auto", strlen("auto")))
		{
			memcpy(fontStyleOut, GSW_CC_FONT_STYLE_EMBEDDED, sizeof(gsw_CcFontStyle));
		}
		else
		{
			AAMPLOG_ERR("%s:%d Unsupported font style type %s", __FUNCTION__, __LINE__, inputStr);
		}
	}
	else
	{
		AAMPLOG_ERR("%s:%d Input is NULL", __FUNCTION__, __LINE__);
		return -1;
	}
	return 0;
}

/**
 * @brief Get edge type value from input string
 *
 * @param[in] input - input edge type value
 * @param[out] fontSizeOut - edge type option for the input value
 * @return int - 0 for success, -1 for failure
 */
static int getEdgeType(std::string input, gsw_CcEdgeType *edgeTypeOut)
{
	AAMPLOG_TRACE("%s:%d input: %s", __FUNCTION__, __LINE__, input.c_str());
	if (!input.empty() && edgeTypeOut)
	{
		const char *inputStr = input.c_str();
		if (0 == strncasecmp(inputStr, "none", strlen("none")))
		{
			*edgeTypeOut = GSW_CC_EDGE_TYPE_NONE;
		}
		else if (0 == strncasecmp(inputStr, "raised", strlen("raised")))
		{
			*edgeTypeOut = GSW_CC_EDGE_TYPE_RAISED;
		}
		else if (0 == strncasecmp(inputStr, "depressed", strlen("depressed")))
		{
			*edgeTypeOut = GSW_CC_EDGE_TYPE_DEPRESSED;
		}
		else if (0 == strncasecmp(inputStr, "uniform", strlen("uniform")))
		{
			*edgeTypeOut = GSW_CC_EDGE_TYPE_UNIFORM;
		}
		else if ((0 == strncasecmp(inputStr, "drop_shadow_left", strlen("drop_shadow_left"))) ||
			(0 == strncasecmp(inputStr, "Left drop shadow", strlen("Left drop shadow"))))
		{
			*edgeTypeOut = GSW_CC_EDGE_TYPE_SHADOW_LEFT;
		}
		else if ((0 == strncasecmp(inputStr, "drop_shadow_right", strlen("drop_shadow_right"))) ||
			(0 == strncasecmp(inputStr, "Right drop shadow", strlen("Right drop shadow"))))
		{
			*edgeTypeOut = GSW_CC_EDGE_TYPE_SHADOW_RIGHT;
		}
		else if (0 == strncasecmp(inputStr, "auto", strlen("auto")))
		{
			*edgeTypeOut = GSW_CC_EDGE_TYPE_EMBEDDED;
		}
		else
		{
			AAMPLOG_ERR("%s:%d Unsupported edge type %s", __FUNCTION__, __LINE__, inputStr);
		}
	}
	else
	{
		AAMPLOG_ERR("%s:%d Input is NULL", __FUNCTION__, __LINE__);
		return -1;
	}
	return 0;
}

/**
 * @brief Get text style value from input string
 *
 * @param[in] input - input text style value
 * @param[out] fontSizeOut - text style option for the input value
 * @return int - 0 for success, -1 for failure
 */
static int getTextStyle(std::string input, gsw_CcTextStyle *textStyleOut)
{
	AAMPLOG_TRACE("%s:%d input: %s", __FUNCTION__, __LINE__, input.c_str());
	if (!input.empty() && textStyleOut)
	{
		const char *inputStr = input.c_str();
		if (0 == strncasecmp(inputStr, "false", strlen("false")))
		{
			*textStyleOut = GSW_CC_TEXT_STYLE_FALSE;
		}
		else if (0 == strncasecmp(inputStr, "true", strlen("true")))
		{
			*textStyleOut = GSW_CC_TEXT_STYLE_TRUE;
		}
		else if (0 == strncasecmp(inputStr, "auto", strlen("auto")))
		{
			*textStyleOut = GSW_CC_TEXT_STYLE_EMBEDDED_TEXT;
		}
		else
		{
			AAMPLOG_ERR("%s:%d Unsupported text style  %s", __FUNCTION__, __LINE__, inputStr);
		}
	}
	else
	{
		AAMPLOG_ERR("%s:%d Input is NULL", __FUNCTION__, __LINE__);
		return -1;
	}
	return 0;
}

/**
 * @brief Get the singleton instance
 *
 * @return AampRDKCCManager - singleton instance
 */
AampRDKCCManager *AampRDKCCManager::GetInstance()
{
	if (mInstance == NULL)
	{
		mInstance = new AampRDKCCManager();
	}
	return mInstance;
}

/**
 * @brief Destroy instance
 *
 * @return void
 */
void AampRDKCCManager::DestroyInstance()
{
	if (mInstance)
	{
		delete mInstance;
		mInstance = NULL;
	}
}

/**
 * @brief Constructor
 */
AampRDKCCManager::AampRDKCCManager():
	mCCHandle(NULL), mEnabled(false), mTrack(), mOptions(),
	mTrickplayStarted(false), mRendering(false)
{

}

/**
 * @brief Destructor
 */
AampRDKCCManager::~AampRDKCCManager()
{

}

/**
 * @brief Initialize CC resource.
 *
 * @param[in] handle - decoder handle
 * @return int - 0 on sucess, -1 on failure
 */
int AampRDKCCManager::Init(void *handle)
{
	int ret = -1;
	static bool initStatus = false;
	if (!initStatus)
	{
		vlGfxInit(0);
		ret = vlMpeosCCManagerInit();
		if (ret != 0)
		{
			return ret;
		}
		initStatus = true;
	}

	if (handle == NULL)
	{
		return -1;
	}

	mCCHandle = handle;
	media_closeCaptionStart((void *)mCCHandle);

	AAMPLOG_WARN("AampRDKCCManager::%s %d Start CC with video dec handle: %p and mEnabled: %d", __FUNCTION__, __LINE__, mCCHandle, mEnabled);

	if (mEnabled)
	{
		Start();
	}
	else
	{
		Stop();
	}

	return ret;
}

/**
 * @brief Release CC resources
 */
void AampRDKCCManager::Release(void)
{
	Stop();
	if (mCCHandle != NULL)
	{
		media_closeCaptionStop();
		mCCHandle = NULL;
	}
	mTrickplayStarted = false;
}


/**
 * @brief Enable/disable CC rendering
 *
 * @param[in] enable - true to enable CC rendering
 * @return int - 0 on success, -1 on failure
 */
int AampRDKCCManager::SetStatus(bool enable)
{
	int ret = 0;
	mEnabled = enable;
	AAMPLOG_WARN("AampRDKCCManager::%s %d mEnabled: %d, mTrickplayStarted: %d, mCCHandle: %s",
				__FUNCTION__, __LINE__, mEnabled, mTrickplayStarted, (mCCHandle != NULL) ? "set" : "not set");
	if (!mTrickplayStarted && mCCHandle != NULL)
	{
		// Setting CC rendering to true before media_closeCaptionStart is not honoured
		// by CC module. CC rendering status is saved in mEnabled and Start/Stop is
		// called when the required operations are completed
		if (mEnabled)
		{
			Start();
		}
		else
		{
			Stop();
		}
	}
	return ret;
}

/**
 * @brief Set CC track
 *
 * @param[in] track - CC track to be selected
 * @return int - 0 on success, -1 on failure
 */
int AampRDKCCManager::SetTrack(const std::string &track, const CCFormat format)
{
	int ret = -1;
	unsigned int trackNum = 0;
	CCFormat finalFormat = eCLOSEDCAPTION_FORMAT_DEFAULT;
	mTrack = track;

	// Check if track is CCx or SERVICEx or track number
	// Could be from 1 -> 63
	if (!track.empty() && track[0] >= CHAR_CODE_1 && track[0] <= CHAR_CODE_6)
	{
		trackNum = (unsigned int) std::stoul(track);
		// This is slightly confusing as we don't know if its 608/708
		// more info might be available in format argument
	}
	else if (track.size() > 2 && aamp_StartsWith(track.c_str(), "CC"))
	{
		// Value between 1 - 8
		// Set as analog channel
		finalFormat = eCLOSEDCAPTION_FORMAT_608;
		trackNum = ((int)track[2] - 48);
	}
	else if (track.size() > 7 && aamp_StartsWith(track.c_str(), "SERVICE"))
	{
		// Value between 1 - 63
		// Set as digital channel
		finalFormat = eCLOSEDCAPTION_FORMAT_708;
		trackNum = (unsigned int) std::stoul(track.substr(7));
	}

	// Format argument overrides whatever we infer from track
	if (format != eCLOSEDCAPTION_FORMAT_DEFAULT && finalFormat != format)
	{
		if (format == eCLOSEDCAPTION_FORMAT_608)
		{
			//Force to 608
			finalFormat = eCLOSEDCAPTION_FORMAT_608;
			if (trackNum > 8)
			{
				//Hope this will not happen in realtime
				trackNum = 0;
			}
			AAMPLOG_INFO("AampRDKCCManager::%s %d Force CC track format to 608 and trackNum to %d!", __FUNCTION__, __LINE__, trackNum);
		}
		else
		{
			//Force to 708
			finalFormat = eCLOSEDCAPTION_FORMAT_708;
			AAMPLOG_INFO("AampRDKCCManager::%s %d Force CC track format to 708!", __FUNCTION__, __LINE__);
		}
	}

	AAMPLOG_WARN("AampRDKCCManager::%s %d Set CC format(%d) trackNum(%d)", __FUNCTION__, __LINE__, finalFormat, trackNum);

	if (finalFormat == eCLOSEDCAPTION_FORMAT_708 && (trackNum > 0 && trackNum <= 63))
	{
		ret = ccSetDigitalChannel(trackNum);
	}
	else if (finalFormat == eCLOSEDCAPTION_FORMAT_608 && (trackNum > 0 && trackNum <= 8))
	{
		int analogChannel = GSW_CC_ANALOG_SERVICE_CC1 + (trackNum - 1);
		ret = ccSetAnalogChannel(analogChannel);
	}
	else
	{
		AAMPLOG_ERR("AampRDKCCManager::%s %d Invalid track number or format, ignoring it!", __FUNCTION__, __LINE__);
	}

	if(0 != ret)
	{
		AAMPLOG_ERR("AampRDKCCManager::%s %d Failed to set trackNum(%d) and format(%d) with ret - %d", __FUNCTION__, __LINE__, trackNum, finalFormat, ret);
	}
	return ret;
}

/**
 * @brief Set CC styles for rendering
 *
 * @param[in] options - rendering style options
 * @return int - 0 on success, -1 on failure
 */
int AampRDKCCManager::SetStyle(const std::string &options)
{
	int ret = -1;
	/*
	 * Values received from JS PP:
	 *
	 * export interface IPlayerCCStyle {
	 *	fontStyle?: string;
	 *	textEdgeColor?: string;
	 *	textEdgeStyle?: string;
	 *	textForegroundColor?: string;
	 *	textForegroundOpacity?: string;
	 *	textBackgroundColor?: string;
	 *	textBackgroundOpacity?: string;
	 *	penItalicized?: string;
	 *	penSize?: string;
	 *	penUnderline?: string;
	 *	windowBorderEdgeColor?: string;
	 *	windowBorderEdgeStyle?: string;
	 *	windowFillColor?: string;
	 *	windowFillOpacity?: string;
	 *	bottomInset?: string;
	 *	}
	 *      FYI - bottomInset unsupported in RDK CC module
	 */

	if (!options.empty())
	{
		AampJsonObject inputOptions(options);
		std::string optionValue;
		ret = 0;
		mOptions = options;

		gsw_CcAttributes attribute;
		/** Get the current attributes */
		ccGetAttributes (&attribute, GSW_CC_TYPE_DIGITAL);

		if (inputOptions.get("fontStyle", optionValue))
		{
			getFontStyle(optionValue, &(attribute.fontStyle));
			ccSetAttributes(&attribute, GSW_CC_ATTRIB_FONT_STYLE, GSW_CC_TYPE_DIGITAL);
			ccSetAttributes(&attribute, GSW_CC_ATTRIB_FONT_STYLE, GSW_CC_TYPE_ANALOG);
		}

		if (inputOptions.get("textEdgeColor", optionValue))
		{
			getColor(GSW_CC_ATTRIB_EDGE_COLOR, GSW_CC_TYPE_DIGITAL, optionValue, &(attribute.edgeColor));
			ccSetAttributes(&attribute, GSW_CC_ATTRIB_EDGE_COLOR, GSW_CC_TYPE_DIGITAL);
			ccSetAttributes(&attribute, GSW_CC_ATTRIB_EDGE_COLOR, GSW_CC_TYPE_ANALOG);
		}

		if (inputOptions.get("textEdgeStyle", optionValue))
		{
			getEdgeType(optionValue, &(attribute.edgeType));
			ccSetAttributes(&attribute, GSW_CC_ATTRIB_EDGE_TYPE, GSW_CC_TYPE_DIGITAL);
			ccSetAttributes(&attribute, GSW_CC_ATTRIB_EDGE_TYPE, GSW_CC_TYPE_ANALOG);
		}

		if (inputOptions.get("textForegroundColor", optionValue))
		{
			getColor(GSW_CC_ATTRIB_FONT_COLOR, GSW_CC_TYPE_DIGITAL, optionValue, &(attribute.charFgColor));
			ccSetAttributes(&attribute, GSW_CC_ATTRIB_FONT_COLOR, GSW_CC_TYPE_DIGITAL);
			ccSetAttributes(&attribute, GSW_CC_ATTRIB_FONT_COLOR, GSW_CC_TYPE_ANALOG);
		}

		if (inputOptions.get("textForegroundOpacity", optionValue))
		{
			getOpacity(optionValue, &(attribute.charFgOpacity));
			ccSetAttributes(&attribute, GSW_CC_ATTRIB_FONT_OPACITY, GSW_CC_TYPE_DIGITAL);
			ccSetAttributes(&attribute, GSW_CC_ATTRIB_FONT_OPACITY, GSW_CC_TYPE_ANALOG);
		}

		if (inputOptions.get("textBackgroundColor", optionValue))
		{
			getColor(GSW_CC_ATTRIB_BACKGROUND_COLOR, GSW_CC_TYPE_DIGITAL, optionValue, &(attribute.charBgColor));
			ccSetAttributes(&attribute, GSW_CC_ATTRIB_BACKGROUND_COLOR, GSW_CC_TYPE_DIGITAL);
			ccSetAttributes(&attribute, GSW_CC_ATTRIB_BACKGROUND_COLOR, GSW_CC_TYPE_ANALOG);
		}

		if (inputOptions.get("textBackgroundOpacity", optionValue))
		{
			getOpacity(optionValue, &(attribute.charBgOpacity));
			ccSetAttributes(&attribute, GSW_CC_ATTRIB_BACKGROUND_OPACITY, GSW_CC_TYPE_DIGITAL);
			ccSetAttributes(&attribute, GSW_CC_ATTRIB_BACKGROUND_OPACITY, GSW_CC_TYPE_ANALOG);
		}

		if (inputOptions.get("penItalicized", optionValue))
		{
			getTextStyle(optionValue, &(attribute.fontItalic));
			ccSetAttributes(&attribute, GSW_CC_ATTRIB_FONT_ITALIC, GSW_CC_TYPE_DIGITAL);
			ccSetAttributes(&attribute, GSW_CC_ATTRIB_FONT_ITALIC, GSW_CC_TYPE_ANALOG);
		}

		if (inputOptions.get("penSize", optionValue))
		{
			getFontSize(optionValue, &(attribute.fontSize));
			ccSetAttributes(&attribute, GSW_CC_ATTRIB_FONT_SIZE, GSW_CC_TYPE_DIGITAL);
			ccSetAttributes(&attribute, GSW_CC_ATTRIB_FONT_SIZE, GSW_CC_TYPE_ANALOG);
		}

		if (inputOptions.get("penUnderline", optionValue))
		{
			getTextStyle(optionValue, &(attribute.fontUnderline));
			ccSetAttributes(&attribute, GSW_CC_ATTRIB_FONT_UNDERLINE, GSW_CC_TYPE_DIGITAL);
			ccSetAttributes(&attribute, GSW_CC_ATTRIB_FONT_UNDERLINE, GSW_CC_TYPE_ANALOG);
		}

		if (inputOptions.get("windowBorderEdgeColor", optionValue))
		{
			getColor(GSW_CC_ATTRIB_BORDER_COLOR, GSW_CC_TYPE_DIGITAL, optionValue, &(attribute.borderColor));
			ccSetAttributes(&attribute, GSW_CC_ATTRIB_BORDER_COLOR, GSW_CC_TYPE_DIGITAL);
			ccSetAttributes(&attribute, GSW_CC_ATTRIB_BORDER_COLOR, GSW_CC_TYPE_ANALOG);
		}

		if (inputOptions.get("windowBorderEdgeStyle", optionValue))
		{
			getEdgeType(optionValue, (gsw_CcEdgeType*) &(attribute.borderType));
			ccSetAttributes(&attribute, GSW_CC_ATTRIB_BORDER_TYPE, GSW_CC_TYPE_DIGITAL);
			ccSetAttributes(&attribute, GSW_CC_ATTRIB_BORDER_TYPE, GSW_CC_TYPE_ANALOG);
		}

		if (inputOptions.get("windowFillColor", optionValue))
		{
			getColor(GSW_CC_ATTRIB_WIN_COLOR, GSW_CC_TYPE_DIGITAL, optionValue, &(attribute.winColor));
			ccSetAttributes(&attribute, GSW_CC_ATTRIB_WIN_COLOR, GSW_CC_TYPE_DIGITAL);
			ccSetAttributes(&attribute, GSW_CC_ATTRIB_WIN_COLOR, GSW_CC_TYPE_ANALOG);
		}

		if (inputOptions.get("windowFillOpacity", optionValue))
		{
			getOpacity(optionValue, &(attribute.winOpacity));
			ccSetAttributes(&attribute, GSW_CC_ATTRIB_WIN_OPACITY, GSW_CC_TYPE_DIGITAL);
			ccSetAttributes(&attribute, GSW_CC_ATTRIB_WIN_OPACITY, GSW_CC_TYPE_ANALOG);
		}
	}
	return ret;
}

/**
 * @brief To enable/disable CC when trickplay starts/ends
 *
 * @param[in] on - true when trickplay starts, false otherwise
 * @return void
 */
void AampRDKCCManager::SetTrickplayStatus(bool on)
{
	AAMPLOG_INFO("AampRDKCCManager::%s %d trickplay status(%d)", __FUNCTION__, __LINE__, on);
	if (on)
	{
		// When trickplay starts, stop CC rendering
		Stop();
	}
	else if (mEnabled)
	{
		// When trickplay ends and CC rendering enabled by app
		Start();
	}
	mTrickplayStarted = on;
}

/**
 * @brief To start CC rendering
 *
 * @return void
 */
void AampRDKCCManager::Start()
{
	AAMPLOG_TRACE("AampRDKCCManager::%s %d mRendering(%d)", __FUNCTION__, __LINE__, mRendering);
	if (!mRendering)
	{
		ccSetCCState(CCStatus_ON, 1);
		mRendering = true;
	}
}

/**
 * @brief To stop CC rendering
 *
 * @return void
 */
void AampRDKCCManager::Stop()
{
	AAMPLOG_TRACE("AampRDKCCManager::%s %d mRendering(%d)", __FUNCTION__, __LINE__, mRendering);
	if (mRendering)
	{
		ccSetCCState(CCStatus_OFF, 1);
		mRendering = false;
	}
}

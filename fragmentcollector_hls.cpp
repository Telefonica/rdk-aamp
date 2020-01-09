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
* \file fragmentcollector_hls.cpp
*
* Fragment Collect file includes class implementation for handling
* HLS streaming of AAMP player. 
* Various functionality like manifest download , fragment collection,
* DRM initialization , synchronizing audio / video media of the stream,
* trick play handling etc are handled in this file .
*
*/
#include "fragmentcollector_hls.h"
#include "_base64.h"
#include "base16.h"
#include <algorithm> // for std::min
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "priv_aamp.h"
#include <pthread.h>
#include <signal.h>
#include <semaphore.h>
#include <math.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <openssl/sha.h>
#include <set>
#include "HlsDrmBase.h"
#include "AampCacheHandler.h"
#ifdef AAMP_VANILLA_AES_SUPPORT
#include "aamp_aes.h"
#endif
#include "webvttParser.h"

//#define TRACE // compile-time optional noisy debug output

#define CHAR_CR 0x0d // '\r'
#define CHAR_LF 0x0a // ''
#define BOOLSTR(boolValue) (boolValue?"true":"false")
#define PLAYLIST_TIME_DIFF_THRESHOLD_SECONDS (0.1f)
#define MAX_DELAY_BETWEEN_PLAYLIST_UPDATE_MS (6*1000)
#define MIN_DELAY_BETWEEN_PLAYLIST_UPDATE_MS (500) //!< 500mSec
#define MAX_LICENSE_ACQ_WAIT_TIME 12000  /*!< 12 secs Increase from 10 to 12 sec(DELIA-33528) */
#define MAX_SEQ_NUMBER_LAG_COUNT 50 /*!< Configured sequence number max count to avoid continuous looping for an edge case scenario, which leads crash due to hung */
#define MAX_SEQ_NUMBER_DIFF_FOR_SEQ_NUM_BASED_SYNC 2 /*!< Maximum difference in sequence number to sync tracks using sequence number.*/
#define DISCONTINUITY_DISCARD_TOLERANCE_SECONDS 30 /*!< Used by discontinuity handling logic to ensure both tracks have discontinuity tag around same area*/
#define MAX_PLAYLIST_REFRESH_FOR_DISCONTINUITY_CHECK_EVENT 5 /*!< Maximum playlist refresh count for discontinuity check for TSB/cDvr*/
#define MAX_PLAYLIST_REFRESH_FOR_DISCONTINUITY_CHECK_LIVE 1 /*!< Maximum playlist refresh count for discontinuity check for live without TSB*/

// checks if current state is going to use IFRAME ( Fragment/Playlist )
#define IS_FOR_IFRAME(rate, type) ((type == eTRACK_VIDEO) && (rate != AAMP_NORMAL_PLAY_RATE))

/**
* \struct	FormatMap
* \brief	FormatMap structure for stream codec/format information 
*/
struct FormatMap
{
	const char* codec;
	StreamOutputFormat format;
};

/// Variable initialization for various audio formats 
static const FormatMap audioFormatMap[AAMP_AUDIO_FORMAT_MAP_LEN] =
{
	{ "mp4a.40.2", FORMAT_AUDIO_ES_AAC },
	{ "mp4a.40.5", FORMAT_AUDIO_ES_AAC },
	{ "ac-3", FORMAT_AUDIO_ES_AC3 },
	{ "mp4a.a5", FORMAT_AUDIO_ES_AC3 },
	{ "ec-3", FORMAT_AUDIO_ES_EC3 },
	{ "ec+3", FORMAT_AUDIO_ES_ATMOS },
	{ "eac3", FORMAT_AUDIO_ES_EC3 }
};

/// Variable initialization for various video formats 
static const FormatMap videoFormatMap[AAMP_VIDEO_FORMAT_MAP_LEN] =
{
	{ "avc1.", FORMAT_VIDEO_ES_H264 },
	{ "hvc1.", FORMAT_VIDEO_ES_HEVC },
	{ "mpeg2v", FORMAT_VIDEO_ES_MPEG2 }//For testing.
};

/// Variable initialization for media profiler buckets 
static const ProfilerBucketType mediaTrackBucketTypes[AAMP_TRACK_COUNT] =
	{PROFILE_BUCKET_FRAGMENT_VIDEO, PROFILE_BUCKET_FRAGMENT_AUDIO, PROFILE_BUCKET_FRAGMENT_SUBTITLE};
/// Variable initialization for media decrypt buckets
static const ProfilerBucketType mediaTrackDecryptBucketTypes[AAMP_DRM_CURL_COUNT] =
	{PROFILE_BUCKET_DECRYPT_VIDEO, PROFILE_BUCKET_DECRYPT_AUDIO};

#ifdef AVE_DRM
extern "C"
{
//setCustomLicensePayload is new extension to AVE's DRM library, required to be populated with Virtual Stream Stitcher (VSS) content 
extern void setCustomLicensePayLoad(const char* customData);
}
#endif
static size_t FindLineLength(const char* ptr);

/***************************************************************************
* @fn startswith
* @brief Function to check if string is start of main string
*        
* @param pstring[in] Main string   
* @param prefix[in] Sub string to check
* @return bool true/false
***************************************************************************/
static bool startswith(char **pstring, const char *prefix)
{
	char *string = *pstring;
	for (;;)
	{
		char c = *prefix++;
		if (c == 0)
		{
			*pstring = string;
			return true;
		}
		if (*string++ != c)
		{
			return false;
		}
	}
}

/***************************************************************************
* @fn AttributeNameMatch
* @brief Function to check if attribute name matches
*        
* @param attrNamePtr[in] Attribute Name pointer   
* @param targetAttrNameCString[in] string to compare
* @return bool true/false
***************************************************************************/
static bool AttributeNameMatch(const char *attrNamePtr, const char *targetAttrNameCString)
{
	while (*targetAttrNameCString)
	{
		if (*targetAttrNameCString++ != *attrNamePtr++)
		{
			return false;
		}
	}
	return *attrNamePtr == '=';
}

/***************************************************************************
* @fn SubStringMatch
* @brief Function to check if substring present in main string 
*        
* @param srcStart[in] Start of string pointer  
* @param srcFin[in] End of string pointer (length = srcFin - srcStart)
* @param cstring[in] string to compare
* @return bool true/false
***************************************************************************/
static bool SubStringMatch(const char *srcStart, const char *srcFin, const char *cstring)
{
	while (srcStart <= srcFin)
	{
		char c = *cstring++;
		if (c == 0)
		{
			return true;
		}
		if (*srcStart != c)
		{
			break;
		}
		srcStart++;
	}
	return false;
}

/***************************************************************************
* @fn GetAttributeValueString
* @brief convert quoted string to NUL-terminated C-string; modifies string in place 
*        
* @param valuePtr[in] quoted string  
* @param fin[in] End of string pointer 
* @return char * pointed to first character of NUL-termianted string
***************************************************************************/
static char * GetAttributeValueString(char *valuePtr, char *fin)
{
	if (*valuePtr == '\"')
	{
		assert(*valuePtr == '\"');
		valuePtr++; // skip begin-quote
		fin--;
		assert(*fin == '\"');
		size_t len = fin - valuePtr;
		valuePtr[len] = 0x00; // replace end-quote with NUL-terminator
	}
	else if (strcmp(valuePtr, "NONE") == 0)
	{
		// patch for http://cilhlsvod.akamaized.net/i/506629/MP4/demo3/abcd123/,cea708test,.mp4.csmil/master.m3u8
		// and watchable content; these use CLOSED_CAPTION=NONE
	}
	else
	{
		logprintf("WARNING: GetAttributeValueString(%s)", valuePtr );
	}
	return valuePtr;
}

/***************************************************************************
* @fn ParseKeyAttributeCallback
* @brief Callback function to decode Key and attribute
*        
* @param attrName[in] input string  
* @param delimEqual[in] delimiter string
* @param fin[in] string end pointer
* @param arg[out] TrackState pointer for storage
* @return void
***************************************************************************/
static void ParseKeyAttributeCallback(char *attrName, char *delimEqual, char *fin, void* arg)
{
	TrackState *ts = (TrackState *)arg;
	char *valuePtr = delimEqual + 1;
	if (AttributeNameMatch(attrName, "METHOD"))
	{
		if (SubStringMatch(valuePtr, fin, "NONE"))
		{ // used by DAI
			if(ts->fragmentEncrypted)
			{
				if (!ts->mIndexingInProgress)
				{
					logprintf("Track %s encrypted to clear ", ts->name);
				}
				ts->fragmentEncrypted = false;
				ts->UpdateDrmCMSha1Hash(NULL);
			}
			ts->mDrmMethod = eDRM_KEY_METHOD_NONE;
		}
		else if (SubStringMatch(valuePtr, fin, "AES-128"))
		{
			if(!ts->fragmentEncrypted)
			{
				if (!ts->mIndexingInProgress)
				{
					AAMPLOG_WARN("Track %s clear to encrypted ", ts->name);
				}
				ts->fragmentEncrypted = true;
			}
			ts->mDrmInfo.method = eMETHOD_AES_128;
			ts->mDrmMethod = eDRM_KEY_METHOD_AES_128;
			ts->mKeyTagChanged = true;
		}
		else if (SubStringMatch(valuePtr, fin, "SAMPLE-AES-CTR"))
		{
			/*
			* TODO- Do license acquisition and license caching meshnism here
			* Time being for fragmented mp4 content, inject fragments as it is and 
			* Let qtdemux do the license acquisition and the decryption part
			*/
                        //AAMPLOG_INFO(" KC - Track %s detected SAMPLE-AES-CTR with protection = %s", 
                        //ts->name, ((ts->fragmentEncrypted)?"TRUE":"FALSE"));
			if(!ts->fragmentEncrypted)
			{
				if (!ts->mIndexingInProgress)
				{
					logprintf("Track %s encrypted to clear \n", ts->name);
				}
				ts->fragmentEncrypted = true;
			}
			ts->mDrmMethod = eDRM_KEY_METHOD_SAMPLE_AES_CTR;
		}
		else if (SubStringMatch(valuePtr, fin, "SAMPLE-AES"))
		{
			ts->mDrmMethod = eDRM_KEY_METHOD_SAMPLE_AES;
			aamp_Error("SAMPLE-AES unsupported");
		}
		else
		{
			ts->mDrmMethod = eDRM_KEY_METHOD_UNKNOWN;
			aamp_Error("unsupported METHOD");
		}
	}
	else if (AttributeNameMatch(attrName, "URI"))
	{
		char *uri = GetAttributeValueString(valuePtr, fin);
		if (ts->mDrmInfo.uri)
		{
			free(ts->mDrmInfo.uri);
		}
		ts->mDrmInfo.uri = strdup(uri);
		//		const char *rkEnd;
		//		const char *rkStart = FindUriAttr(this->mDrmInfo.uri, "EncryptedRK", &rkEnd);
		//		if (rkStart)
		//		{ // ParseHexData(mDrmInfo.encryptedRotationKey, 16, rkStart, rkEnd);
		//			aamp_Error("EncryptedRK unsupported");
		//		}
	}
	else if (AttributeNameMatch(attrName, "IV"))
	{ // 16 bytes
		const char *srcPtr = valuePtr;
		assert(srcPtr[0] == '0');
		assert(srcPtr[1] == 'x');
		srcPtr += 2;
		ts->UpdateDrmIV(srcPtr);
	}
	else if (AttributeNameMatch(attrName, "CMSha1Hash"))
	{ // 20 bytes; Metadata Hash.
		assert(valuePtr[0] == '0');
		assert(valuePtr[1] == 'x');
		ts->UpdateDrmCMSha1Hash(valuePtr+2);
	}
}


/***************************************************************************
* @fn ParseXStartAttributeCallback
* @brief Callback function to decode XStart attributes
*		 
* @param attrName[in] input string	
* @param delimEqual[in] delimiter string
* @param fin[in] string end pointer
* @param arg[out] TrackState pointer for storage
* @return void
***************************************************************************/
static void ParseXStartAttributeCallback(char *attrName, char *delimEqual, char *fin, void* arg)
{
	HLSXStart *var = (HLSXStart *)arg;
	char *valuePtr = delimEqual + 1;
	if (AttributeNameMatch(attrName, "TIME-OFFSET"))
	{
		var->offset = atof(valuePtr);
	}
	else if (AttributeNameMatch(attrName, "PRECISE"))
	{
		// Precise attribute is not considered . By default NO option is selected 	
		var->precise = false;
	}
}

/***************************************************************************
* @fn ParseStreamInfCallback
* @brief Callback function to extract stream tag attributes
*        
* @param attrName[in] input string  
* @param delimEqual[in] delimiter string
* @param fin[in] string end pointer
* @param arg[out] StreamAbstractionAAMP_HLS pointer for storage
* @return void
***************************************************************************/
static void ParseStreamInfCallback(char *attrName, char *delimEqual, char *fin, void* arg)
{
	StreamAbstractionAAMP_HLS *context = (StreamAbstractionAAMP_HLS *) arg;
	char *valuePtr = delimEqual + 1;
	HlsStreamInfo *streamInfo = &context->streamInfo[context->GetProfileCount()];
	if (AttributeNameMatch(attrName, "URI"))
	{
		streamInfo->uri = GetAttributeValueString(valuePtr, fin);
	}
	else if (AttributeNameMatch(attrName, "BANDWIDTH"))
	{
		streamInfo->bandwidthBitsPerSecond = atol(valuePtr);
	}
	else if (AttributeNameMatch(attrName, "PROGRAM-ID"))
	{
		streamInfo->program_id = atol(valuePtr);
	}
	else if (AttributeNameMatch(attrName, "AUDIO"))
	{
		streamInfo->audio = GetAttributeValueString(valuePtr, fin);
	}
	else if (AttributeNameMatch(attrName, "CODECS"))
	{
		streamInfo->codecs = GetAttributeValueString(valuePtr, fin);
	}
	else if (AttributeNameMatch(attrName, "RESOLUTION"))
	{
		sscanf(delimEqual + 1, "%dx%d", &streamInfo->resolution.width, &streamInfo->resolution.height);
	}
	// following are rarely present
	else if (AttributeNameMatch(attrName, "AVERAGE-BANDWIDTH"))
	{
		streamInfo->averageBandwidth = atol(valuePtr);
	}
	else if (AttributeNameMatch(attrName, "FRAME-RATE"))
	{
		streamInfo->resolution.framerate = atof(valuePtr);
	}
	else if (AttributeNameMatch(attrName, "CLOSED-CAPTIONS"))
	{
		streamInfo->closedCaptions = GetAttributeValueString(valuePtr, fin);
	}
	else if (AttributeNameMatch(attrName, "SUBTITLES"))
	{
		streamInfo->subtitles = GetAttributeValueString(valuePtr, fin);
	}
	else 
	{
		AAMPLOG_INFO("unknown stream inf attribute %s", attrName);
	}
}

/***************************************************************************
* @fn ParseMediaAttributeCallback
* @brief Callback function to extract media tag attributes
*		 
* @param attrName[in] input string	
* @param delimEqual[in] delimiter string
* @param fin[in] string end pointer
* @param arg[out] StreamAbstractionAAMP_HLS pointer for storage
* @return void
***************************************************************************/
static void ParseMediaAttributeCallback(char *attrName, char *delimEqual, char *fin, void *arg)
{
	StreamAbstractionAAMP_HLS *context = (StreamAbstractionAAMP_HLS *) arg;
	char *valuePtr = delimEqual + 1;
	struct MediaInfo *mediaInfo = &context->mediaInfo[context->GetMediaCount()];
	/*
	#EXT - X - MEDIA:TYPE = AUDIO, GROUP - ID = "g117600", NAME = "English", LANGUAGE = "en", DEFAULT = YES, AUTOSELECT = YES
	#EXT - X - MEDIA:TYPE = AUDIO, GROUP - ID = "g117600", NAME = "Spanish", LANGUAGE = "es", URI = "HBOHD_HD_NAT_15152_0_5939026565177792163/format-hls-track-sap-bandwidth-117600-repid-root_audio103.m3u8"
	*/
	if (AttributeNameMatch(attrName, "TYPE"))
	{
		if (SubStringMatch(valuePtr, fin, "AUDIO"))
		{
			mediaInfo->type = eMEDIATYPE_AUDIO;
		}
		else if (SubStringMatch(valuePtr, fin, "VIDEO"))
		{
			mediaInfo->type = eMEDIATYPE_VIDEO;
		}
		else if (SubStringMatch(valuePtr, fin, "SUBTITLES"))
		{
			mediaInfo->type = eMEDIATYPE_SUBTITLE;
		}
	}
	else if (AttributeNameMatch(attrName, "GROUP-ID"))
	{
		mediaInfo->group_id = GetAttributeValueString(valuePtr, fin);
	}
	else if (AttributeNameMatch(attrName, "NAME"))
	{
		mediaInfo->name = GetAttributeValueString(valuePtr, fin);
	}
	else if (AttributeNameMatch(attrName, "LANGUAGE"))
	{
		mediaInfo->language = GetAttributeValueString(valuePtr, fin);
	}
	else if (AttributeNameMatch(attrName, "AUTOSELECT"))
	{
		if (SubStringMatch(valuePtr, fin, "YES"))
		{
			mediaInfo->autoselect = true;
		}
	}
	else if (AttributeNameMatch(attrName, "DEFAULT"))
	{
		if (SubStringMatch(valuePtr, fin, "YES"))
		{
			mediaInfo->isDefault = true;
		}
	}
	else if (AttributeNameMatch(attrName, "URI"))
	{
		mediaInfo->uri = GetAttributeValueString(valuePtr, fin);
	}
	// rarely present
	else if (AttributeNameMatch(attrName, "CHANNELS"))
	{
		mediaInfo->channels = atoi(valuePtr);
	}
	else if (AttributeNameMatch(attrName, "INSTREAM-ID"))
	{
		mediaInfo->instreamID = GetAttributeValueString(valuePtr, fin);
	}
	else if (AttributeNameMatch(attrName, "FORCED"))
	{
		if (SubStringMatch(valuePtr, fin, "YES"))
		{
			mediaInfo->forced = true;
		}
	}
	else
	{
		logprintf("unk MEDIA attr %s", attrName);
	}
}

/***************************************************************************
* @fn mystrpbrk
* @brief Function to extract string pointer afer LF
*		 
* @param ptr[in] input string	
* @return char * - Next string pointer
***************************************************************************/
static char *mystrpbrk(char *ptr)
{ // lines are terminated by either a single LF character or a CR character followed by an LF character
	char *next = NULL;
	char *fin = strchr(ptr, CHAR_LF);
	if (fin)
	{
		next = fin + 1;
		//handles lines terminated by CR characters followed by LF character
		while(fin > ptr && fin[-1] == CHAR_CR)
		{
			fin--;
		}
		*fin = 0x00;
	}
	return next;
}

/**
* @param attrName pointer to HLS Attribute List, as a NUL-terminated CString
*/
/***************************************************************************
* @fn ParseAttrList
* @brief Function to parse attribute list from tag string 
*		 
* @param attrName[in] input string	
* @param cb[in] callback function to store parsed attributes
* @param context[in] void pointer context
* @return void
***************************************************************************/
static void ParseAttrList(char *attrName, void(*cb)(char *attrName, char *delim, char *fin, void *context), void *context)
{
	while (*attrName)
	{
		while (*attrName == ' ')
		{ // strip whitespace
			attrName++;
		}
		char *delimEqual = attrName;
		// break out on CR
		if(*delimEqual == '\r')
			break;
		while (*delimEqual != '=')
		{ // An AttributeName is an unquoted string containing characters from the set [A..Z] and '-'
			char c = *delimEqual++;
			(void)c;
			// assert((c >= 'A' && c <= 'Z') || c == '-'); // breaks when CMSha1Hash used
		}
		char *fin = delimEqual;
		bool inQuote = false;
		for (;;)
		{
			char c = *fin;
			if (c == 0)
			{
				break;
			}
			else if (c == '\"')
			{
				if (inQuote)
				{
					inQuote = false;
					fin++;
					break;
				}
				else
				{
					inQuote = true;
				}
			}
			else if (c == ',' && !inQuote)
			{
				break;
			}
			fin++;
		}
		cb(attrName, delimEqual, fin, context);
		if (*fin == ',')
		{
			fin++;
		}
		attrName = fin;
	}
}

static bool ParseTimeFromProgramDateTime(const char* ptr, struct timeval &programDateTimeVal )
{
	bool retVal = false;
	struct tm timeinfo;
	int ms = 0;
	memset(&timeinfo, 0, sizeof(timeinfo));
	/* discarding timezone assuming audio and video tracks has same timezone and we use this time only for synchronization*/
	int ret = sscanf(ptr, "%d-%d-%dT%d:%d:%d.%d", &timeinfo.tm_year, &timeinfo.tm_mon,
			&timeinfo.tm_mday, &timeinfo.tm_hour, &timeinfo.tm_min, &timeinfo.tm_sec, &ms);
	if (ret >= 6)
	{
		timeinfo.tm_year -= 1900;
		programDateTimeVal.tv_sec = mktime(&timeinfo);
		programDateTimeVal.tv_usec = ms * 1000;
		retVal = true;
	}
	else
	{
		logprintf("Parse error on DATE-TIME: %*s ret = %d", 30, ptr, ret);
	}
	return retVal;
}
/***************************************************************************
* @fn ParseXStartTimeOffset
* @brief Helper function to Parse XStart Tag and attributes
*
* @param arg[in] char *ptr , string with X-START
* @return double offset value
***************************************************************************/
static double ParseXStartTimeOffset(const char* ptr)
{
	double retOffSet = 0.0;
	if(ptr)
	{
		size_t len = FindLineLength(ptr);
		char* xstartStr =(char*) malloc (len+1);
		if(xstartStr)
		{
			memcpy(xstartStr,ptr,len);
			xstartStr[len]='\0';

			HLSXStart xstart;
			ParseAttrList(xstartStr, ParseXStartAttributeCallback, &xstart);
			free(xstartStr);
			retOffSet = xstart.offset;
		}
	}
	return retOffSet;
}


/***************************************************************************
* @fn TrackPLDownloader
* @brief Thread function for download 
*		 
* @param arg[in] void ptr , thread arguement
* @return void ptr
***************************************************************************/
static void * TrackPLDownloader(void *arg)
{
	TrackState* ts = (TrackState*)arg;
	if(aamp_pthread_setname(pthread_self(), "aampAudPL"))
	{
		logprintf("%s:%d: aamp_pthread_setname failed", __FUNCTION__, __LINE__);
	}
	ts->FetchPlaylist();
	return NULL;
}

/***************************************************************************
* @fn ParseMainManifest
* @brief Function to parse main manifest 
*		 
* @param ptr[in] Manifest file content string
*	
* @return AAMPStatusType
***************************************************************************/
AAMPStatusType StreamAbstractionAAMP_HLS::ParseMainManifest(char *ptr)
{
	AAMPStatusType retval = eAAMPSTATUS_OK;
	bool secondPass = false;
	// Get the initial configuration to filter the profiles
	bool bDisableEC3 = gpGlobalConfig->disableEC3;
	// bringing in parity with DASH , if EC3 is disabled ,then ATMOS also will be disabled
	bool bDisableATMOS = (gpGlobalConfig->disableEC3)?true:gpGlobalConfig->disableATMOS;
	bool bDisableAAC = false;

	bool ignoreProfile = false,clearProfiles = false;
	int aacProfiles=0,ec3Profiles=0,atmosProfiles=0;
	int vProfileCount ,iFrameCount ,lineNum ;
	// tmp buffer to retrieve the manifest if second pass needed
	GrowableBuffer tmpMainManifest;
	// Main manifest contents
	// Case 1: Handled
	//	Media , Stream Profile (Single Codec) , IFrame Profiles ( In Order)
	// Case 2: Handled
	//	Media , Stream Profile (Multi Codec) , IFrame Profiles (In Order)
	// Case 3: Handled
	//	Media , Stream(Single Codec) and IFrame profiles mixed up ( not in order)
	// Case 4. Handled
	//	Media , Stream(Multi Codec) and IFrame profiles mixed up( not in order)

	// Priority of Profile selection if no filter set : ATMOS , EAC3 , AAC .
	do{
	int aacProfiles=0,ec3Profiles=0,atmosProfiles=0;
	mMediaCount = 0;
	vProfileCount = iFrameCount = lineNum = 0;
	mAbrManager.clearProfiles();
	secondPass = false;
#ifdef AVE_DRM
	//clear previouse data
	setCustomLicensePayLoad(NULL);
#endif
	while (ptr)
	{
		char *next = mystrpbrk(ptr);
		if (*ptr)
		{
			if (startswith(&ptr, "#EXT"))
			{
				if (startswith(&ptr, "-X-I-FRAME-STREAM-INF:"))
				{
					HlsStreamInfo *streamInfo = &this->streamInfo[GetProfileCount()];
					memset(streamInfo, 0, sizeof(*streamInfo));
					ParseAttrList(ptr, ParseStreamInfCallback, this);
					if (streamInfo->uri == NULL)
					{ // uri on following line
						streamInfo->uri = next;
						next = mystrpbrk(next);
					}
					streamInfo->isIframeTrack = true;
					mAbrManager.addProfile({
						streamInfo->isIframeTrack,
						streamInfo->bandwidthBitsPerSecond,
						streamInfo->resolution.width,
						streamInfo->resolution.height,
					});
					iFrameCount++;					
				}
				else if (startswith(&ptr, "-X-STREAM-INF:"))
				{
					ignoreProfile = false;
					struct HlsStreamInfo *streamInfo = &this->streamInfo[GetProfileCount()];
					memset(streamInfo, 0, sizeof(HlsStreamInfo));
					ParseAttrList(ptr, ParseStreamInfCallback, this);
					if (streamInfo->uri == NULL)
					{ // uri on following line
						streamInfo->uri = next;
						next = mystrpbrk(next);
					}
					if(streamInfo->codecs) {
					// First Check point : if AAC profile. Commonly available profile , so checked first
					if(strstr(streamInfo->codecs,"mp4a.40.2") || (strstr(streamInfo->codecs,"mp4a.40.5")))
					{
						if(bDisableAAC)
						{
							AAMPLOG_WARN("%s:%d: AAC Profile ignored[%s]", __FUNCTION__, __LINE__, streamInfo->uri);
							ignoreProfile = true;
						}
						else
						{
							aacProfiles++;
						}
					}
					// next check point on ec3 profiles
					else if(strstr(streamInfo->codecs,"ec-3")|| (strstr(streamInfo->codecs,"eac3")))
					{
						if(bDisableEC3)
						{
							AAMPLOG_WARN("%s:%d: EC3 Profile ignored[%s]", __FUNCTION__, __LINE__, streamInfo->uri);
							ignoreProfile = true;
						}
						else
						{
							// found EC3 profile , disable AAC profiles from adding
							ec3Profiles++;
							bDisableAAC = true;
							if(aacProfiles)
							{
								// if already aac profiles added , clear it from local table and ABR table
								aacProfiles = 0;
								clearProfiles = true;
							}
						}
					}
					// next checkpoint on atmos profiles
					else if(strstr(streamInfo->codecs,"ec+3"))
					{
						if(bDisableATMOS)
						{
							AAMPLOG_WARN("%s:%d: ATMOS Profile ignored[%s]", __FUNCTION__, __LINE__, streamInfo->uri);
							ignoreProfile = true;
						}
						else
						{
							// found ATMOS Profile , disable EC3 and AAC profile from adding
							atmosProfiles++;
							bDisableAAC = true;
							bDisableEC3 = true;
							if(aacProfiles || ec3Profiles)
							{
								// if already aac or ec3 profiles added , clear it from local table and ABR table
								aacProfiles = ec3Profiles = 0;
								clearProfiles = true;
							}
						}
					}
					else
					{
						AAMPLOG_WARN("%s:%d unknown codec string to categorize :%s ",__FUNCTION__,__LINE__,streamInfo->codecs);
					}

					if(clearProfiles)
					{
						vProfileCount = 0;
						clearProfiles = false;
						// some profiles which was already added to abr need to be cleared
						// if stream only profiles were parsed , its easy to flush them out and continue with parsing
						// but if iframes are mixed along with video profile, then its tricky to remove the interleaved entries from StreamInfo
						// Easiest way is to clearAllProfiles and reparse again
						// SecondParse happens rarely in some streams , no impact on regular Comcast streams
						if(iFrameCount)
						{
							// IFrame and Stream Profile mixed, only way is reparse from beginning
							// Retrieve the MainManifest content from cache .
							memset(&tmpMainManifest, 0, sizeof(tmpMainManifest));
							std::string tmpUrlString;
							aamp->getAampCacheHandler()->RetrieveFromPlaylistCache(aamp->GetManifestUrl(), &tmpMainManifest, tmpUrlString);
							if (tmpMainManifest.len)
							{
								ptr = tmpMainManifest.ptr;
								secondPass = true;
							}
							else
							{
								mAbrManager.clearProfiles();
							}
							break;
						}
						else
						{
							// this will clear only video profiles already added in abr
							// no iframes added yet ,hence no need to update StreamInfo table
							mAbrManager.clearProfiles();
						}
					}
					}// end of if for codec checking

					// add profile only if ignore is not set
					if(!ignoreProfile)
					{
						mAbrManager.addProfile({
							streamInfo->isIframeTrack,
							streamInfo->bandwidthBitsPerSecond,
							streamInfo->resolution.width,
							streamInfo->resolution.height
						});
						vProfileCount++;
					}
				}
				else if (startswith(&ptr, "-X-MEDIA:"))
				{
					memset(&this->mediaInfo[mMediaCount], 0, sizeof(MediaInfo));
					ParseAttrList(ptr, ParseMediaAttributeCallback, this);
					mMediaCount++;
				}
				else if (startswith(&ptr, "-X-VERSION:"))
				{
					// followed by integer
				}
				else if (startswith(&ptr, "-X-INDEPENDENT-SEGMENTS"))
				{
					// followed by integer
				}
				else if (startswith(&ptr, "-X-FAXS-CM"))
				{ // not needed - present in playlist
					hasDrm = true;
				}
				else if (startswith(&ptr, "M3U"))
				{
					// Spec :: 4.3.1.1.  EXTM3U - It MUST be the first line of every Media Playlist and every Master Playlist
					if(lineNum)
					{
						logprintf("%s:%d M3U tag not the first line[%d] of Manifest",__FUNCTION__,__LINE__,lineNum);
						retval = eAAMPSTATUS_MANIFEST_CONTENT_ERROR;
						break;
					}
				}
				else if (startswith(&ptr, "-X-CONTENT-IDENTIFIER:"))
				{
#ifdef AVE_DRM
					std::string vssServiceZone = aamp->getServiceZone();

					if(!vssServiceZone.empty())
					{
						char * ptrContentID = ptr;

						if(startswith(&ptr, "urn:merlin:linear:stream:"))
						{
							std::string vssData = " \"client:accessAttributes\": [\"content:xcal:virtualStreamId\",\"";
							vssData.append(ptr);
							vssData.append("\",\"device:xcal:serviceZone\",\"");
							vssData.append(vssServiceZone);
							vssData.append("\"]");
							setCustomLicensePayLoad(vssData.c_str());
							AAMPLOG_INFO("%s:%d: custom vss data:%s", __FUNCTION__, __LINE__,vssData.c_str());
						}
						else
						{
							AAMPLOG_WARN("%s:%d: Invalid VirtualStreamID:%s", __FUNCTION__, __LINE__, ptrContentID);
						}
					}
#endif
				}
				else if (startswith(&ptr, "-X-FOG"))
				{
				}
				else if (startswith(&ptr, "-X-XCAL-CONTENTMETADATA"))
				{ // placeholder for new Super8 DRM Agnostic Metadata
				}
				else if (startswith(&ptr, "-NOM-I-FRAME-DISTANCE"))
				{ // placeholder for nominal distance between IFrames
				}
				else if (startswith(&ptr, "-X-ADVERTISING"))
				{ // placeholder for advertising zone for linear (soon to be deprecated)
				}
				else if (startswith(&ptr, "-UPLYNK-LIVE"))
				{ // related to uplynk streaming service
				}
				else if (startswith(&ptr, "-X-START:"))					
				{ // i.e. "TIME-OFFSET=2.336, PRECISE=YES" - specifies the preferred point in the video to start playback; not yet supported

                                       // check if App has not configured any liveoffset
                                        if(!aamp->mNewLiveOffsetflag)
                                        {
                                                double offsetval = ParseXStartTimeOffset(ptr);
						if (offsetval != 0)
						{
                                                	if(!aamp->IsLiveAdjustRequired())
                                                	{
                                                        	// if aamp cfg offset is not set or App has not set the value  , then configure
                                                        	if(gpGlobalConfig->cdvrliveOffset == -1)
                                                                	aamp->mLiveOffset = abs(offsetval);
                                                	}
                                               		else
                                                	{
                                                        	// if aamp cfg offset is not set or App has not set the value , then configure
                                                        	if(gpGlobalConfig->liveOffset == -1)
									aamp->mLiveOffset = abs(offsetval);
                                                	}
							logprintf("%s WARNING:found EXT-X-START in MainManifest Offset:%f  liveOffset:%f",__FUNCTION__,offsetval,aamp->mLiveOffset);
						}
                                        }
				}
				else if (startswith(&ptr, "INF:"))
				{
					// its not a main manifest, instead its playlist given for playback . Consider not a error
					// Report it , so that Init flow can be changed accordingly 
					retval = eAAMPSTATUS_MANIFEST_INVALID_TYPE;
					break;
				}
				else 
				{
					std::string unknowTag= ptr;
					AAMPLOG_INFO("***unknown tag:%s", unknowTag.substr(0,24).c_str());
				}
				lineNum++;
			}
		}
		ptr = next;
	}// while till end of file
	}while(secondPass);

	if(retval == eAAMPSTATUS_OK)
	{
		// Check if there are are valid profiles to do playback 
		if(vProfileCount == 0)
		{
			logprintf("%s:%d ERROR No video profiles available in manifest for playback",__FUNCTION__,__LINE__);
			retval = eAAMPSTATUS_MANIFEST_CONTENT_ERROR;
		}
		else
		{	// If valid video profiles , check for Media and iframe and report warnings only 
			if(mMediaCount == 0)
			{
				// just a warning .Play the muxed content with default audio 
				logprintf("%s:%d WARNING !!! No media definitions in manifest for playback",__FUNCTION__,__LINE__);
			}
			if(iFrameCount == 0)
			{
				// just a warning 
				logprintf("%s:%d WARNING !!! No iframe definitions .Trickplay not supported",__FUNCTION__,__LINE__);
			}
			else
			{
				UpdateIframeTracks();
			}
		}
	}
	return retval;
} // ParseMainManifest


/***************************************************************************
* @fn GetFragmentUriFromIndex
* @brief Function to get fragment URI from index count
*		 
* @return string fragment URI pointer
***************************************************************************/
char *TrackState::GetFragmentUriFromIndex()
{
	char * uri = NULL;
	const IndexNode *index = (IndexNode *) this->index.ptr;
	const IndexNode *idxNode = NULL;
	int idx;
	if (context->rate > 0)
	{

		const IndexNode *lastIndexNode = &index[indexCount - 1];
		double seekWindowEnd = lastIndexNode->completionTimeSecondsFromStart - aamp->mLiveOffset; 
		if (IsLive() && playTarget > seekWindowEnd)
		{
			logprintf("%s - rate - %f playTarget(%f) > seekWindowEnd(%f), forcing EOS",
                                __FUNCTION__, context->rate, playTarget, seekWindowEnd);
			return NULL;
		}

		if (currentIdx == -1)
		{ // search forward from beginning
			currentIdx = 0;
		}
		for (idx = currentIdx; idx < indexCount; idx++)
		{ // search in direction until out-of-bounds
			const IndexNode *node = &index[idx];
			//logprintf("%s rate %f completionTimeSecondsFromStart %f playTarget %f",__FUNCTION__, rate, node->completionTimeSecondsFromStart, playTarget);
			if (node->completionTimeSecondsFromStart >= playTarget)
			{ // found target iframe
#ifdef TRACE
				logprintf("%s Found node - rate %f completionTimeSecondsFromStart %f playTarget %f", __FUNCTION__,
				        context->rate, node->completionTimeSecondsFromStart, playTarget);
#endif
				idxNode = node;
				break;
			}
		}
	}
	else
	{
		if (-1 == currentIdx)
		{ // search backward from end
			currentIdx = indexCount - 1;
		}
		for (idx = currentIdx; idx >= 0; idx--)
		{ // search in direction until out-of-bounds
			const IndexNode *node = &index[idx];
			//logprintf("%s rate %f completionTimeSecondsFromStart %f playTarget %f",__FUNCTION__, rate, node->completionTimeSecondsFromStart, playTarget);
			if (node->completionTimeSecondsFromStart <= playTarget)
			{ // found target iframe
#ifdef TRACE
				logprintf("%s Found node - rate %f completionTimeSecondsFromStart %f playTarget %f",
						__FUNCTION__, context->rate, node->completionTimeSecondsFromStart, playTarget);
#endif

				idxNode = node;
				break;
			}
		}
	}
	if (idxNode)
	{
		currentIdx = idx;
		byteRangeOffset = 0;
		byteRangeLength = 0;
		//logprintf("%s fragmentinfo %s", __FUNCTION__, idxNode->pFragmentInfo);
		const char *fragmentInfo = idxNode->pFragmentInfo;
		fragmentDurationSeconds = idxNode->completionTimeSecondsFromStart;
		if (idx > 0)
		{
			fragmentDurationSeconds -= index[idx - 1].completionTimeSecondsFromStart;
		}
		while (fragmentInfo[0] == '#')
		{
			if (!memcmp(fragmentInfo, "#EXT-X-BYTERANGE:", 17))
			{
				char temp[1024];
				const char * end = fragmentInfo;
				while (end[0] != CHAR_LF)
				{
					end++;
				}
				int len = end - fragmentInfo;
				assert(len < 1024);
				strncpy(temp, fragmentInfo + 17, len);
				temp[1023] = 0x00;
				char * offsetDelim = strchr(temp, '@'); // optional
				if (offsetDelim)
				{
					*offsetDelim++ = 0x00;
					byteRangeOffset = atoi(offsetDelim);
				}
				byteRangeLength = atoi(temp);
			}
			/*Skip to next line*/
			while (fragmentInfo[0] != CHAR_LF)
			{
				fragmentInfo++;
			}
			fragmentInfo++;
		}
		const char *urlEnd = strchr(fragmentInfo, CHAR_LF);
		if (urlEnd)
		{
			if (*(urlEnd - 1) == CHAR_CR)
			{
				urlEnd--;
			}
			int urlLen = urlEnd - fragmentInfo;
			mFragmentURIFromIndex.assign(fragmentInfo, urlLen);
			if(!mFragmentURIFromIndex.empty()){
				uri = (char *)mFragmentURIFromIndex.c_str();
			}
		}
		else
		{
			logprintf("%s - unable to find end", __FUNCTION__);
		}
		if (-1 == idxNode->drmMetadataIdx)
		{
			fragmentEncrypted = false;
		}
		else
		{
			fragmentEncrypted = true;
			// for each iframe , need to see if KeyTag changed and get the drminfo .
			// Get the key Index position .
			int keyIndexPosn = idxNode->drmMetadataIdx;
			if(keyIndexPosn != mLastKeyTagIdx)
			{
				logprintf("%s:%d:[%d] KeyTable Size [%d] keyIndexPosn[%d] lastKeyIdx[%d]",__FUNCTION__,__LINE__,type,mKeyHashTable.size(),keyIndexPosn,mLastKeyTagIdx);
				if(keyIndexPosn < mKeyHashTable.size() && mKeyHashTable[keyIndexPosn].mKeyTagStr.size())
				{
					// ParseAttrList function modifies the input string ,hence cannot pass mKeyTagStr
					// modifying const memory will cause crash . So had to copy locally
					char* key =(char*) malloc (mKeyHashTable[keyIndexPosn].mKeyTagStr.size());
                                        memcpy(key,mKeyHashTable[keyIndexPosn].mKeyTagStr.c_str(),mKeyHashTable[keyIndexPosn].mKeyTagStr.size());

					//logprintf("%s:%d:[%d] Parse the Key attribute for new KeyIndex[%d][%s] ",__FUNCTION__,__LINE__,type,keyIndexPosn,mKeyHashTable[keyIndexPosn].mShaID.c_str());
					ParseAttrList((char *)key, ParseKeyAttributeCallback, this);
					free(key);
				}
				mKeyTagChanged = true;
				mLastKeyTagIdx = keyIndexPosn;
			}

		}
	}
	else
	{
		logprintf("%s - Couldn't find node - rate %f playTarget %f",
				__FUNCTION__, context->rate, playTarget);
	}
	return uri;
}

/***************************************************************************
* @fn GetNextFragmentUriFromPlaylist
* @brief Function to get next fragment URI from playlist based on playtarget
* @param ignoreDiscontinuity Ignore discontinuity
* @return string fragment URI pointer
***************************************************************************/
char *TrackState::GetNextFragmentUriFromPlaylist(bool ignoreDiscontinuity)
{
	char *ptr = fragmentURI;
	char *rc = NULL;
	int byteRangeLength = 0; // default, when optional byterange offset is left unspecified
	int byteRangeOffset = 0;
	bool discontinuity = false;
	const char* programDateTime = NULL;

	traceprintf ("GetNextFragmentUriFromPlaylist : playTarget %f playlistPosition %f fragmentURI %p", playTarget, playlistPosition, fragmentURI);
	if (playTarget < 0)
	{
		logprintf("%s - invalid playTarget %f ", __FUNCTION__, playTarget);
		playTarget = 0;
		//return fragmentURI; // leads to buffer overrun/crash
	}
	if (playlistPosition == playTarget)
	{
		//logprintf("[PLAYLIST_POSITION==PLAY_TARGET]");
		return fragmentURI;
	}
	if (playlistPosition != -1)
	{ // already presenting - skip past previous segment
		//logprintf("[PLAYLIST_POSITION!= -1]");
		ptr += strlen(fragmentURI) + 1;
	}
	if ((playlistPosition > playTarget) && (fragmentDurationSeconds > PLAYLIST_TIME_DIFF_THRESHOLD_SECONDS) &&
		((playlistPosition - playTarget) > fragmentDurationSeconds))
	{
		logprintf("%s - playlistPosition[%f] > playTarget[%f] more than last fragmentDurationSeconds[%f]",
					__FUNCTION__, playlistPosition, playTarget, fragmentDurationSeconds);
	}
	if (-1 == playlistPosition)
	{
		// Starts parsing from beginning, so change to default
		fragmentEncrypted = false;
	}
	//logprintf("%s: before loop, ptr = %p fragmentURI %p", __FUNCTION__, ptr, fragmentURI);
	while (ptr)
	{
		char *next = mystrpbrk(ptr);
		//logprintf("ptr %s next %.*s", ptr, 10, next);
		if (*ptr)
		{
			if (startswith(&ptr, "#EXT"))
			{ // tags begins with #EXT
				if (startswith(&ptr, "M3U"))
				{ // "Extended M3U file" - always first line
				}
				else if (startswith(&ptr, "INF:"))
				{// preceeds each advertised fragment in a playlist
					if (-1 != playlistPosition)
					{
						playlistPosition += fragmentDurationSeconds;
					}
					else
					{
						playlistPosition = 0;
					}
					fragmentDurationSeconds = atof(ptr);
#ifdef TRACE
					logprintf("Next - EXTINF - playlistPosition updated to %f", playlistPosition);
					// optionally followed by human-readable title
#endif
				}
				else if (startswith(&ptr, "-X-BYTERANGE:"))
				{
					char  temp[1024];
					strncpy(temp, ptr, 1023);
					temp[1023] = 0x00;
					char * offsetDelim = strchr(temp, '@'); // optional
					if (offsetDelim)
					{
						*offsetDelim++ = 0x00;
						byteRangeOffset = atoi(offsetDelim);
					}
					byteRangeLength = atoi(temp);
					mByteOffsetCalculation = true;
					if (0 != byteRangeLength && 0 == byteRangeOffset)
					{
						byteRangeOffset = this->byteRangeOffset + this->byteRangeLength;
					}
					AAMPLOG_TRACE("%s:%d byteRangeOffset:%d Last played fragment Offset:%d byteRangeLength:%d Last fragment Length:%d", __FUNCTION__,__LINE__, byteRangeOffset, this->byteRangeOffset, byteRangeLength, this->byteRangeLength);
				}
				else if (startswith(&ptr, "-X-TARGETDURATION:"))
				{ // max media segment duration; required; appears once
					targetDurationSeconds = atof(ptr);
				}
				else if (startswith(&ptr, "-X-MEDIA-SEQUENCE:"))
				{// first media URI's unique integer sequence number
					nextMediaSequenceNumber = atoll(ptr);
				}
				else if (startswith(&ptr, "-X-KEY:"))
				{ // identifies licensing server to contact for authentication
					ParseAttrList(ptr, ParseKeyAttributeCallback, this);
				}
				else if(startswith(&ptr,"-X-MAP:"))
				{
					AAMPLOG_TRACE("%s:%d: Old-Init : %s, New-Init:%s", __FUNCTION__, __LINE__, mInitFragmentInfo, ptr);
					if ((!mInitFragmentInfo) || (mInitFragmentInfo && ptr && strcmp(mInitFragmentInfo, ptr) != 0))
					{
						mInitFragmentInfo = ptr;
						mInjectInitFragment = true;
						AAMPLOG_INFO("%s:%d: Found #EXT-X-MAP data: %s", __FUNCTION__, __LINE__, mInitFragmentInfo);
					}
				}
				else if (startswith(&ptr, "-X-PROGRAM-DATE-TIME:"))
				{ // associates following media URI with absolute date/time
					// if used, should supplement any EXT-X-DISCONTINUITY tags
					AAMPLOG_TRACE("Got EXT-X-PROGRAM-DATE-TIME: %s ", ptr);
					if (context->mNumberOfTracks > 1)
					{
						programDateTime = ptr;
						// The first X-PROGRAM-DATE-TIME tag holds the start time for each track
						if (startTimeForPlaylistSync.tv_sec == 0 && startTimeForPlaylistSync.tv_usec == 0)
						{
							/* discarding timezone assuming audio and video tracks has same timezone and we use this time only for synchronization*/
							bool ret = ParseTimeFromProgramDateTime(ptr, startTimeForPlaylistSync);
							if (ret)
							{
								AAMPLOG_TRACE("DATE-TIME: %s startTime updated to %ld.%06ld",
										ptr ,startTimeForPlaylistSync.tv_sec,
										(long)startTimeForPlaylistSync.tv_usec);
							}
						}
					}
				}
				else if (startswith(&ptr, "-X-ALLOW-CACHE:"))
				{ // YES or NO - authorizes client to cache segments for later replay
					if (startswith(&ptr, "YES"))
					{
						context->allowsCache = true;
					}
					else if (startswith(&ptr, "NO"))
					{
						context->allowsCache = false;
					}
					else
					{
						aamp_Error("unknown ALLOW-CACHE setting");
					}
				}
				else if (startswith(&ptr, "-X-PLAYLIST-TYPE:"))
				{
					//PlaylistType is handled during indexing.
				}
				else if (startswith(&ptr, "-X-ENDLIST"))
				{ // indicates that no more media segments are available
					logprintf("#EXT-X-ENDLIST");
					mReachedEndListTag = true;
				}
				else if (startswith(&ptr, "-X-DISCONTINUITY-SEQUENCE"))
				{
					// ignore this tag for now 
				}
				else if (startswith(&ptr, "-X-DISCONTINUITY"))
				{
					discontinuity = true;
				}
				else if (startswith(&ptr, "-X-I-FRAMES-ONLY"))
				{
					logprintf("#EXT-X-I-FRAMES-ONLY");
				}
				else if (startswith(&ptr, "-X-VERSION:"))
				{
					int version = atoi(ptr);
					(void)version;
				}
				// custom tags follow:
				else if (startswith(&ptr, "-X-FAXS-CM:"))
				{
					//DRM meta data is stored during indexing.
				}
				else if (startswith(&ptr, "-X-FAXS-PACKAGINGCERT"))
				{
				}
				else if (startswith(&ptr, "-X-FAXS-SIGNATURE"))
				{
				}
				else if (startswith(&ptr, "-X-CUE"))
				{
				}
				else if (startswith(&ptr, "-X-CM-SEQUENCE"))
				{
				}
				else if (startswith(&ptr, "-X-MARKER"))
				{
				}
				else if (startswith(&ptr, "-X-MAP"))
				{
				}
				else if (startswith(&ptr, "-X-MEDIA-TIME"))
				{
				}
				else if (startswith(&ptr, "-X-END-TOP-TAGS"))
				{
				}
				else if (startswith(&ptr, "-X-CONTENT-IDENTIFIER"))
				{
				}
				else if (startswith(&ptr, "-X-TRICKMODE-RESTRICTION"))
				{
				}
				else if (startswith(&ptr, "-X-INDEPENDENT-SEGMENTS"))
				{
				}
				else if (startswith(&ptr, "-X-BITRATE"))
				{
				}
				else if (startswith(&ptr, "-X-FOG"))
				{
				}
				else if (startswith(&ptr,"-UPLYNK-LIVE"))
				{//tag related to uplynk streaming service
				}
				else if (startswith(&ptr, "-X-START:"))
				{
				}
				else if (startswith(&ptr, "-X-XCAL-CONTENTMETADATA"))
				{ // placeholder for new Super8 DRM Agnostic Metadata
				}
				else if (startswith(&ptr, "-NOM-I-FRAME-DISTANCE"))
				{ // placeholder for nominal distance between IFrames
				}
				else if (startswith(&ptr, "-X-ADVERTISING"))
				{ // placeholder for advertising zone for linear (soon to be deprecated)
				}
				else if (startswith(&ptr, "-X-SOURCE-STREAM"))
				{ // placeholder for vss source stream id
				}
				else if (startswith(&ptr, "-X-X1-LIN-CK"))
				{ // placeholder for deferred drm information
				}
				else if (startswith(&ptr, "-X-SCTE35"))
				{ // placeholder for DAI tag processing
				}
				else 
				{
					std::string unknowTag= ptr;
					AAMPLOG_INFO("***unknown tag:%s", unknowTag.substr(0,24).c_str());	
				}
			}
			else if (*ptr == '#')
			{ // all other lines beginning with # are comments
			}
			else if (*ptr == '\0')
			{ // skip inserted null char
			}
			else
			{ // URI
				nextMediaSequenceNumber++;
				if (((playlistPosition + fragmentDurationSeconds) > playTarget) || ((playTarget - playlistPosition) < PLAYLIST_TIME_DIFF_THRESHOLD_SECONDS))
				{
					//logprintf("Return fragment %s playlistPosition %f playTarget %f", ptr, playlistPosition, playTarget);
					this->byteRangeOffset = byteRangeOffset;
					this->byteRangeLength = byteRangeLength;
					mByteOffsetCalculation = false;
					if (discontinuity)
					{
						if (!ignoreDiscontinuity)
						{
							logprintf("%s:%d #EXT-X-DISCONTINUITY in track[%d] playTarget %f total mCulledSeconds %f", __FUNCTION__, __LINE__, type, playTarget, mCulledSeconds);
							TrackType otherType = (type == eTRACK_VIDEO)? eTRACK_AUDIO: eTRACK_VIDEO;
							TrackState *other = context->trackState[otherType];
							if (other->enabled)
							{
								double diff;
								double position;
								double playPosition = playTarget - mCulledSeconds;
								if (!programDateTime)
								{
									position = playPosition;
								}
								else
								{
									struct timeval programDateTimeVal;
									bool ret = ParseTimeFromProgramDateTime(programDateTime, programDateTimeVal);
									if (ret)
									{
										AAMPLOG_TRACE("DATE-TIME: %s startTime updated to %ld.%06ld",
												ptr ,programDateTimeVal.tv_sec,
												(long)programDateTimeVal.tv_usec);
									}
									position = programDateTimeVal.tv_sec + (double)programDateTimeVal.tv_usec/1000000;
									logprintf("%s:%d [%s] Discontinuity - position from program-date-time %f", __FUNCTION__, __LINE__, name, position);
								}
								if (!other->HasDiscontinuityAroundPosition(position, (NULL != programDateTime), diff, playPosition))
								{
									logprintf("%s:%d [%s] Ignoring discontinuity as %s track does not have discontinuity", __FUNCTION__, __LINE__, name, other->name);
									discontinuity = false;
								}
								else if (programDateTime)
								{
									logprintf("%s:%d [%s] diff %f ", __FUNCTION__, __LINE__, name, diff);
									/*If other track's discontinuity is in advanced position, diff is positive*/
									if (diff > fragmentDurationSeconds/2 )
									{
										/*Skip fragment*/
										logprintf("%s:%d [%s] Discontinuity - other track's discontinuity time greater by %f. updating playTarget %f to %f",
												__FUNCTION__, __LINE__, name, diff, playTarget, playlistPosition + diff);
										mSyncAfterDiscontinuityInProgress = true;
										playTarget = playlistPosition + diff;
										discontinuity = false;
										programDateTime = NULL;
										ptr = next;
										continue;
									}
								}
							}
						}
						else
						{
							discontinuity = false;
						}
					}
					this->discontinuity = discontinuity || mSyncAfterDiscontinuityInProgress;
					mSyncAfterDiscontinuityInProgress = false;
					traceprintf("%s:%d [%s] Discontinuity - %d", __FUNCTION__, __LINE__, name, (int)this->discontinuity);
					rc = ptr;
					break;
				}
				else
				{
					if(mByteOffsetCalculation)
					{
						byteRangeOffset += byteRangeLength;
					}
					discontinuity = false;
					programDateTime = NULL;
					// logprintf("Skipping fragment %s playlistPosition %f playTarget %f", ptr, playlistPosition, playTarget);
				}
			}
		}
		ptr = next;
	}
#ifdef TRACE
	logprintf("GetNextFragmentUriFromPlaylist :  pos %f returning %s", playlistPosition, rc);
	logprintf("seqNo=%lld", nextMediaSequenceNumber - 1);
#endif
	return rc;
}


/***************************************************************************
* @fn FindMediaForSequenceNumber
* @brief Get fragment tag based on media sequence number 
*		 
* @return string fragment tag line pointer 
***************************************************************************/
char *TrackState::FindMediaForSequenceNumber()
{
	char *ptr = playlist.ptr;
	long long mediaSequenceNumber = nextMediaSequenceNumber - 1;
	char *key = NULL;
	char *initFragment = NULL;

	long long seq = 0;
	while (ptr)
	{
		char *next = mystrpbrk(ptr);
		if (*ptr)
		{
			if (startswith(&ptr, "#EXTINF:"))
			{
				fragmentDurationSeconds = atof(ptr);
			}
			else if (startswith(&ptr, "#EXT-X-MEDIA-SEQUENCE:"))
			{
				seq = atoll(ptr);
			}
			else if (startswith(&ptr, "#EXT-X-KEY:"))
			{
				key = ptr;
			}
			else if (startswith(&ptr, "#EXT-X-MAP:"))
			{
				initFragment = ptr;
			}
			else if (ptr[0] != '#')
			{ // URI
				if (seq >= mediaSequenceNumber)
				{
					if ((mDrmKeyTagCount >1) && key)
					{
						ParseAttrList(key, ParseKeyAttributeCallback, this);
					}
					if (initFragment)
					{
						// mInitFragmentInfo will be cleared after calling FlushIndex() from IndexPlaylist()
						if (!mInitFragmentInfo)
						{
							mInitFragmentInfo = initFragment;
							AAMPLOG_INFO("%s:%d: Found #EXT-X-MAP data: %s", __FUNCTION__, __LINE__, mInitFragmentInfo);
						}
					}
					if (seq != mediaSequenceNumber)
					{
						logprintf("seq gap %lld!=%lld", seq, mediaSequenceNumber);
						nextMediaSequenceNumber = seq + 1;
					}
					return ptr;
				}
				seq++;
			}
		}
		ptr = next;
	}
	return NULL;
}
/***************************************************************************
* @fn FetchFragmentHelper
* @brief Helper function to download fragment 
*		 
* @param http_error[out] http error string 	
* @param decryption_error[out] decryption error
* @return bool true on success else false 
***************************************************************************/
bool TrackState::FetchFragmentHelper(long &http_error, bool &decryption_error, bool & bKeyChanged, int * fogError)
{
#ifdef TRACE
		logprintf("FetchFragmentHelper Enter: pos %f start %f frag-duration %f fragmentURI %s",
				playlistPosition, playTarget, fragmentDurationSeconds, fragmentURI );
#endif
		assert (fragmentURI);
		if (context->trickplayMode && ABRManager::INVALID_PROFILE != context->GetIframeTrack())
		{
			fragmentURI = GetFragmentUriFromIndex();
			double delta = context->rate / context->mTrickPlayFPS;
			if (context->rate < 0)
			{ // rewind
				if (!fragmentURI || (playTarget == 0))
				{
					logprintf("aamp rew to beginning");
					eosReached = true;
				}
				else if (playTarget > -delta)
				{
					playTarget += delta;
				}
				else
				{
					playTarget = 0;
				}
			}
			else
			{// fast-forward
				if (!fragmentURI)
				{
					logprintf("aamp ffw to end");
					eosReached = true;
				}
				playTarget += delta;
			}
			//logprintf("Updated playTarget to %f", playTarget);
		}
		else
		{// normal speed
			fragmentURI = GetNextFragmentUriFromPlaylist();
			if (fragmentURI != NULL)
			{
				if (!mInjectInitFragment)
					playTarget = playlistPosition + fragmentDurationSeconds;
				if (IsLive())
				{
					context->CheckForPlaybackStall(true);
				}
			}
			else
			{
				if ((!IsLive() || mReachedEndListTag) && (playlistPosition != -1))
				{
					logprintf("aamp play to end. playTarget %f fragmentURI %p ReachedEndListTag %d Type %d", playTarget, fragmentURI, mReachedEndListTag,type);
					eosReached = true;
				}
				else if (IsLive() && type == eTRACK_VIDEO)
				{
					context->CheckForPlaybackStall(false);
				}
			}
		}

		if (!mInjectInitFragment && fragmentURI)
		{
			std::string fragmentUrl;
			CachedFragment* cachedFragment = GetFetchBuffer(true);
			aamp_ResolveURL(fragmentUrl, mEffectiveUrl, fragmentURI);
			traceprintf("Got next fragment url %s fragmentEncrypted %d discontinuity %d mDrmMethod %d", fragmentUrl, fragmentEncrypted, (int)discontinuity, mDrmMethod);

			aamp->profiler.ProfileBegin(mediaTrackBucketTypes[type]);
			const char *range;
			char rangeStr[128];
			if (byteRangeLength)
			{
				int next = byteRangeOffset + byteRangeLength;
				sprintf(rangeStr, "%d-%d", byteRangeOffset, next - 1);
				logprintf("FetchFragmentHelper rangeStr %s ", rangeStr);

				range = rangeStr;
			}
			else
			{
				range = NULL;
			}
#ifdef TRACE
			logprintf("FetchFragmentHelper: fetching %s", fragmentUrl.c_str());
#endif
			// patch for http://bitdash-a.akamaihd.net/content/sintel/hls/playlist.m3u8
			// if fragment URI uses relative path, we don't want to replace effective URI
			std::string tempEffectiveUrl;
			traceprintf("%s:%d Calling Getfile . buffer %p avail %d", __FUNCTION__, __LINE__, &cachedFragment->fragment, (int)cachedFragment->fragment.avail);
			bool fetched = aamp->GetFile(fragmentUrl, &cachedFragment->fragment, tempEffectiveUrl, &http_error, range, type, false, (MediaType)(type));
			//Workaround for 404 of subtitle fragments
			//TODO: This needs to be handled at server side and this workaround has to be removed
			if (!fetched && http_error == 404 && type == eTRACK_SUBTITLE)
			{
				aamp_AppendBytes(&cachedFragment->fragment, "WEBVTT", 7);
				fetched = true;
			}
			if (!fetched)
			{
				//cleanup is done in aamp_GetFile itself

				aamp->profiler.ProfileError(mediaTrackBucketTypes[type], http_error);
				segDLFailCount += 1;
				if (AAMP_IS_LOG_WORTHY_ERROR(http_error))
				{
					logprintf("FetchFragmentHelper aamp_GetFile failed");
				}
				//Adding logic to report error if fragment downloads are failing continuously
				//Avoid sending error for failure to download subtitle fragments
				if(MAX_SEG_DOWNLOAD_FAIL_COUNT <= segDLFailCount && aamp->DownloadsAreEnabled() && type != eTRACK_SUBTITLE)
				{
					logprintf("Not able to download fragments; reached failure threshold sending tune failed event");
					aamp->SendDownloadErrorEvent(AAMP_TUNE_FRAGMENT_DOWNLOAD_FAILURE, http_error);
				}
				aamp_Free(&cachedFragment->fragment.ptr);
				return false;
			}

			if((eTRACK_VIDEO == type)  && (aamp->IsTSBSupported()))
			{
				std::size_t pos = fragmentUrl.find(FOG_FRAG_BW_IDENTIFIER);
				if (pos != std::string::npos)
				{
					std::string bwStr = fragmentUrl.substr(pos + FOG_FRAG_BW_IDENTIFIER_LEN);
					if (!bwStr.empty())
					{
						pos = bwStr.find(FOG_FRAG_BW_DELIMITER);
						if (pos != std::string::npos)
						{
							bwStr = bwStr.substr(0, pos);
							context->SetTsbBandwidth(std::stol(bwStr));
						}
					}
				}
			}

			aamp->profiler.ProfileEnd(mediaTrackBucketTypes[type]);
			segDLFailCount = 0;

			if (cachedFragment->fragment.len && fragmentEncrypted && mDrmMethod == eDRM_KEY_METHOD_AES_128)
			{
				// DrmDecrypt resets mKeyTagChanged , take a back up here to give back to caller
				bKeyChanged = mKeyTagChanged;
				{	
					traceprintf("%s:%d [%s] uri %s - calling  DrmDecrypt()", __FUNCTION__, __LINE__, name, fragmentURI);
					DrmReturn drmReturn = DrmDecrypt(cachedFragment, mediaTrackDecryptBucketTypes[type]);

					if(eDRM_SUCCESS != drmReturn)
					{
						if (aamp->DownloadsAreEnabled())
						{
							logprintf("FetchFragmentHelper : drm_Decrypt failed. fragmentURI %s - RetryCount %d", fragmentURI, segDrmDecryptFailCount);
							if (eDRM_KEY_ACQUSITION_TIMEOUT == drmReturn)
							{
								decryption_error = true;
								logprintf("FetchFragmentHelper : drm_Decrypt failed due to license acquisition timeout");
								aamp->SendErrorEvent(AAMP_TUNE_LICENCE_TIMEOUT, NULL, true);
							}
							else
							{
								/* Added to send tune error when fragments decryption failed */
								segDrmDecryptFailCount +=1;

								if(MAX_SEG_DRM_DECRYPT_FAIL_COUNT <= segDrmDecryptFailCount)
								{
									decryption_error = true;
									logprintf("FetchFragmentHelper : drm_Decrypt failed for fragments, reached failure threshold sending failure event");
									aamp->SendErrorEvent(AAMP_TUNE_DRM_DECRYPT_FAILED);
								}
							}
						}
						aamp_Free(&cachedFragment->fragment.ptr);
						return false;
					}
#ifdef TRACE
					else
					{
						logprintf("aamp: hls - eMETHOD_AES_128 not set for %s", fragmentURI);
					}
#endif
					segDrmDecryptFailCount = 0; /* Resetting the retry count in the case of decryption success */
				}
#ifdef AAMP_HARVEST_SUPPORT_ENABLED
				context->HarvestFile(fragmentUrl, &cachedFragment->fragment, true);
#endif
				if (!context->firstFragmentDecrypted)
				{
					aamp->NotifyFirstFragmentDecrypted();
					context->firstFragmentDecrypted = true;
				}
			}
			else if(!cachedFragment->fragment.len)
			{
				logprintf("fragment. len zero for %s", fragmentURI);
			}
#ifdef AAMP_HARVEST_SUPPORT_ENABLED
			else
			{
				context->HarvestFile(fragmentUrl, &cachedFragment->fragment, true);
			}
#endif
		}
		else
		{
			bool ret = false;
			if (mInjectInitFragment)
			{
				AAMPLOG_INFO("FetchFragmentHelper : Found init fragment playTarget(%f), playlistPosition(%f)", playTarget, playlistPosition);
				ret = true; // we need to ret success here to avoid failure cases in FetchFragment
			}
			else
			{
				// null fragment URI technically not an error - live manifest may simply not have updated yet
				// if real problem exists, underflow will eventually be detected/reported
				AAMPLOG_INFO("FetchFragmentHelper : fragmentURI %s playTarget(%f), playlistPosition(%f)", fragmentURI, playTarget, playlistPosition);
			}
			return ret;
		}
		return true;
}
/***************************************************************************
* @fn FetchFragment
* @brief Function to fetch fragment  
*		 
* @return void
***************************************************************************/
void TrackState::FetchFragment()
{
	int timeoutMs = -1;
	long http_error = 0;
	bool decryption_error = false;
	if (IsLive())
	{
		timeoutMs = context->maxIntervalBtwPlaylistUpdateMs - (int) (aamp_GetCurrentTimeMS() - lastPlaylistDownloadTimeMS);
		if(timeoutMs < 0)
		{
			timeoutMs = 0;
		}
	}
	if (!WaitForFreeFragmentAvailable(timeoutMs))
	{
		return;
	}
	AAMPLOG_INFO("%s:%d: %s", __FUNCTION__, __LINE__, name);
	//DELIA-33346 -- always set the rampdown flag to false .
	context->mCheckForRampdown = false;
        bool bKeyChanged = false;
        int iFogErrorCode = -1;
	int iCurrentRate = aamp->rate; //  Store it as back up, As sometimes by the time File is downloaded, rate might have changed due to user initiated Trick-Play
	if (false == FetchFragmentHelper(http_error, decryption_error,bKeyChanged,&iFogErrorCode) && aamp->DownloadsAreEnabled() )
	{
		if (fragmentURI )
		{
			// DELIA-32287 - Profile RampDown check and rampdown is needed only for Video . If audio fragment download fails
			// should continue with next fragment,no retry needed .
			if (eTRACK_VIDEO == type && http_error != 0)
			{
				context->lastSelectedProfileIndex = context->currentProfileIndex;
				if (context->CheckForRampDownProfile(http_error))
				{
					if (context->rate == AAMP_NORMAL_PLAY_RATE)
					{
						playTarget -= fragmentDurationSeconds;
					}
					else
					{
						playTarget -= context->rate / context->mTrickPlayFPS;
					}
				}
				logprintf("%s:%d: Error while fetching fragment:%s, failedCount:%d. decrementing profile", __FUNCTION__, __LINE__, name, segDLFailCount);
				//DELIA-33346 -- if rampdown attempted , then set the flag so that abr is not attempted . 
				context->mCheckForRampdown = true;
			}
			else if (decryption_error)
			{
				logprintf("%s:%d: Error while decrypting fragments. failedCount:%d", __FUNCTION__, __LINE__, segDLFailCount);
			}
			else if (AAMP_IS_LOG_WORTHY_ERROR(http_error))
			{
				logprintf("%s:%d: Error on fetching %s fragment. failedCount:%d", __FUNCTION__, __LINE__, name, segDLFailCount);
			}
		}
		else
		{
			// technically not an error - live manifest may simply not have updated yet
			// if real problem exists, underflow will eventually be detected/reported
			AAMPLOG_TRACE("%s:%d: NULL fragmentURI for %s track ", __FUNCTION__, __LINE__, name);
		}

		// in case of tsb, GetCurrentBandWidth does not return correct bandwidth as it is updated after this point
		// hence getting from context which is updated in FetchFragmentHelper
		long lbwd = aamp->IsTSBSupported() ? context->GetTsbBandwidth() : this->GetCurrentBandWidth() * 8;
		//update videoend info
		aamp->UpdateVideoEndMetrics( (IS_FOR_IFRAME(iCurrentRate,type)? eMEDIATYPE_IFRAME:(MediaType)(type) ),
								lbwd,
								((iFogErrorCode > 0 ) ? iFogErrorCode : http_error),this->mEffectiveUrl,fragmentDurationSeconds,bKeyChanged,fragmentEncrypted);

		return;
	}

	if (mInjectInitFragment)
	{
		return;
	}

	CachedFragment* cachedFragment = GetFetchBuffer(false);
	if (cachedFragment->fragment.ptr)
	{
		double duration = fragmentDurationSeconds;
		double position = playTarget - playTargetOffset;
		if (type == eTRACK_SUBTITLE)
		{
			aamp_AppendNulTerminator(&cachedFragment->fragment);
		}
		if (context->rate == AAMP_NORMAL_PLAY_RATE)
		{
			position -= fragmentDurationSeconds;
			cachedFragment->discontinuity = discontinuity;
		}
		else
		{
			position -= context->rate / context->mTrickPlayFPS;
			cachedFragment->discontinuity = true;
			traceprintf("%s:%d: rate %f position %f",__FUNCTION__, __LINE__, context->rate, position);
		}

		if (context->trickplayMode && (0 != context->rate))
		{
			duration = (int)(duration*context->rate / context->mTrickPlayFPS);
		}
		cachedFragment->duration = duration;
		cachedFragment->position = position;

		// in case of tsb, GetCurrentBandWidth does not return correct bandwidth as it is updated after this point
		// hence getting from context which is updated in FetchFragmentHelper
		long lbwd = aamp->IsTSBSupported() ? context->GetTsbBandwidth() : this->GetCurrentBandWidth() * 8;

		//update videoend info
		aamp->UpdateVideoEndMetrics( (IS_FOR_IFRAME(iCurrentRate,type)? eMEDIATYPE_IFRAME:(MediaType)(type) ),
								lbwd,
								((iFogErrorCode > 0 ) ? iFogErrorCode : http_error),this->mEffectiveUrl,cachedFragment->duration,bKeyChanged,fragmentEncrypted);
	}
	else
	{
		logprintf("%s:%d: %s cachedFragment->fragment.ptr is NULL", __FUNCTION__, __LINE__, name);
	}
#ifdef AAMP_DEBUG_INJECT
	if ((1 << type) & AAMP_DEBUG_INJECT)
	{
		strcpy(cachedFragment->uri, fragmentURI);
	}
#endif
	mSkipAbr = false; //To enable ABR since we have cached fragment after init fragment
	UpdateTSAfterFetch();
}
/***************************************************************************
* @fn InjectFragmentInternal
* @brief Injected decrypted fragment for playback 
*		 
* @param cachedFragment[in] CachedFragment structure 	
* @param fragmentDiscarded[out] bool to indicate fragment successfully injected
* @return void
***************************************************************************/
void TrackState::InjectFragmentInternal(CachedFragment* cachedFragment, bool &fragmentDiscarded)
{
#ifndef SUPRESS_DECODE
#ifndef FOG_HAMMER_TEST // support aamp stress-tests of fog without video decoding/presentation
	if (playContext)
	{
		double position = 0;
		if(!context->mStartTimestampZero)
		{
			position = cachedFragment->position;
		}
		fragmentDiscarded = !playContext->sendSegment(cachedFragment->fragment.ptr, cachedFragment->fragment.len,
				position, cachedFragment->duration, cachedFragment->discontinuity, ptsError);
	}
	else
	{
		fragmentDiscarded = false;
		aamp->SendStream((MediaType)type, cachedFragment->fragment.ptr, cachedFragment->fragment.len,
		        cachedFragment->position, cachedFragment->position, cachedFragment->duration);
	}
#endif
#endif
} // InjectFragmentInternal
/***************************************************************************
* @fn GetCompletionTimeForFragment
* @brief Function to get end time of fragment
*		 
* @param trackState[in] TrackState structure	
* @param mediaSequenceNumber[in] sequence number 
* @return double end time 
***************************************************************************/
static double GetCompletionTimeForFragment(const TrackState *trackState, long long mediaSequenceNumber)
{
	double rc = 0.0;
	int indexCount = trackState->indexCount;
	if (indexCount>0)
	{
		int idx = (int)(mediaSequenceNumber - trackState->indexFirstMediaSequenceNumber);
		if (idx >= 0)
		{
			if (idx >= indexCount)
			{
				idx = indexCount - 1;
			}
			const IndexNode *node = &((IndexNode *)trackState->index.ptr)[idx];
			rc = node->completionTimeSecondsFromStart;
		}
		else
		{
			AAMPLOG_WARN("%s:%d bad index! mediaSequenceNumber=%lld, indexFirstMediaSequenceNumber=%lld", __FUNCTION__, __LINE__, mediaSequenceNumber, trackState->indexFirstMediaSequenceNumber);
		}
	}
	return rc;
}

#ifdef TRACE
/***************************************************************************
* @fn DumpIndex
* @brief Function to log stored media information 
*		 
* @param trackState[in] TrackState structure
* @return void
***************************************************************************/
static void DumpIndex(TrackState *trackState)
{
	logprintf("index (%d fragments)", trackState->indexCount);
	long long mediaSequenceNumber = trackState->indexFirstMediaSequenceNumber;
	for (int idx = 0; idx < trackState->indexCount; idx++)
	{
		const IndexNode *node = &((IndexNode *)trackState->index.ptr)[idx];
		logprintf("%lld: %f %d", mediaSequenceNumber, node->completionTimeSecondsFromStart, node->drmMetadataIdx);
		mediaSequenceNumber++;
	}
}
#endif
/***************************************************************************
* @fn FlushIndex
* @brief Function to flush all stored data before refresh and stop
*		 
* @return void
***************************************************************************/
void TrackState::FlushIndex()
{
	aamp_Free(&index.ptr);
	indexFirstMediaSequenceNumber = 0;
	indexCount = 0;
	index.len = 0;
	index.avail = 0;
	currentIdx = -1;
	mDrmKeyTagCount = 0;
	mLastKeyTagIdx = -1;
	mDeferredDrmKeyMaxTime = 0;
	mKeyHashTable.clear();
	mDiscontinuityIndexCount = 0;
	aamp_Free(&mDiscontinuityIndex.ptr);
	memset(&mDiscontinuityIndex, 0, sizeof(mDiscontinuityIndex));
	if (mDrmMetaDataIndexCount)
	{
		traceprintf("TrackState::%s:%d [%s]mDrmMetaDataIndexCount %d", __FUNCTION__, __LINE__, name,
		        mDrmMetaDataIndexCount);
		DrmMetadataNode* drmMetadataNode = (DrmMetadataNode*) mDrmMetaDataIndex.ptr;
		assert(NULL != drmMetadataNode);
		for (int i = 0; i < mDrmMetaDataIndexCount; i++)
		{
			traceprintf("TrackState::%s:%d drmMetadataNode[%d].metaData.metadataPtr %p", __FUNCTION__, __LINE__, i,
			        drmMetadataNode[i].metaData.metadataPtr);

			if ((NULL == drmMetadataNode[i].metaData.metadataPtr || NULL == drmMetadataNode[i].sha1Hash) && mDrmMetaDataIndexCount)
			{
				logprintf ("TrackState::%s:%d **** metadataPtr/sha1Hash is NULL, give attention and analyze it... mDrmMetaDataIndexCount[%d]", __FUNCTION__, __LINE__, mDrmMetaDataIndexCount);
			}

			if (drmMetadataNode[i].metaData.metadataPtr)
			{
				free(drmMetadataNode[i].metaData.metadataPtr);
				drmMetadataNode[i].metaData.metadataPtr = NULL;
			}

			if (drmMetadataNode[i].sha1Hash)
			{
				free(drmMetadataNode[i].sha1Hash);
				drmMetadataNode[i].sha1Hash = NULL;
			}
		}
		aamp_Free(&mDrmMetaDataIndex.ptr);
		memset(&mDrmMetaDataIndex, 0, sizeof(mDrmMetaDataIndex));
		mDrmMetaDataIndexCount = 0;
		mDrmMetaDataIndexPosition = 0;
	}
	mInitFragmentInfo = NULL;
}

/***************************************************************************
* @fn ComputeDeferredKeyRequestTime
* @brief Function to compute Deferred key request time for VSS Stream Meta data
***************************************************************************/
void TrackState::ComputeDeferredKeyRequestTime()
{
	// This function will be called only if special tag -X-X1-LIN is present to differ the Key acquisition
	//  on random value based on the Max refresh time interval
	// From gathered information , Meta data gets added to playlist and then Key tag follows after certain time interval
	// defined in X-X1-LIN tag . So if a new Meta appears , before the KeyTag comes up , Key need to be acquired on a random
	// timeout.
	// Assumption 1:There is no deferring required between 2 or more meta present in the playlist if there is equivalent KeyTag
	// 				with Sha is present .For all the Metas with KeyTags , key request will happen immediately
	// Assumption 2 not considered : Not sure if new Meta gets added always at the end or can get it added in any index ,
	//			 either by Fog or Source server. So going worst case, if it can added any position , need to run the loop completely

	bool foundFlag = false;
	if(mDrmMetaDataIndexCount > 1)
	{
		DrmMetadataNode* drmMetadataNode = (DrmMetadataNode*)mDrmMetaDataIndex.ptr;
		int foundMetaCounter = 0;
		for (int idx = 0; idx < mDrmMetaDataIndexCount; idx++)
		{
			// Check if the Meta is already present with AveDrmManager. If there its already configured for deferred time,no need to add again
			// If not present , check if Key Tag is present or not .
			// a)if Key tag present , then no deferring . Key requested immediately.
			// b)if Key tag not present , then calculate deferred time
			if(AveDrmManager::IsMetadataAvailable(drmMetadataNode[idx].sha1Hash) == -1)
			{
				foundFlag = false;
				// checking Hash availability in KeyTags
				KeyHashTableIter iter = mKeyHashTable.begin();
				for(;iter != mKeyHashTable.end();iter++)
				{
					if(iter->mShaID.size() && (0 == memcmp(iter->mShaID.c_str(), drmMetadataNode[idx].sha1Hash , DRM_SHA1_HASH_LEN)))
					{
						// Key tag present , not to defer
						foundFlag = true;
						foundMetaCounter++;
						break;
					}
				}

				if(!foundFlag)
				{
					// Found new Meta with no key Mapping. Need to defer key request for this Meta
					int deferredTimeMs = GetDeferTimeMs(drmMetadataNode[idx].deferredInterval);
					drmMetadataNode[idx].drmKeyReqTime = aamp_GetCurrentTimeMS() + deferredTimeMs;

					logprintf("[%s][%d][%s] Found New Meta[%d] without KeyTag mapping.Defer license request[%d]",
						__FUNCTION__,__LINE__,name,idx,deferredTimeMs);
				}
				else
				{
					// This is preventive measure to avoid overloading of DRM and cpu.
					// For any reason process crashes and comes back ,fog may give complete TSB with so many
					// Metadata with KeyTag available. This will cause sudden rush of Meta add and key requset
					// Differ the key in staggered manner. If particular Meta-Key is needed for decrypt, then it will
					// be requested on Emergency mode
					if(foundMetaCounter > 2)
					{
						int deferredTimeMs = GetDeferTimeMs(30);
						drmMetadataNode[idx].drmKeyReqTime = aamp_GetCurrentTimeMS() + deferredTimeMs;
						logprintf("[%s][%d][%s] Found New Meta[%d] with KeyTag mapping.Deferring license request due to load[%d]",__FUNCTION__,__LINE__,name,idx,deferredTimeMs);
					}
				}
			}
		}
	}
}

/***************************************************************************
* @fn ProcessDrmMetadata
* @brief Process Drm Metadata after indexing
***************************************************************************/
void TrackState::ProcessDrmMetadata()
{

	// This function after indexplaylist to be called , this will store the metadata into AveDrmManager
	// Storing will not invoke key request . If meta already present , no update happens
	DrmMetadataNode* drmMetadataNode = (DrmMetadataNode*)mDrmMetaDataIndex.ptr;
	if(mDrmMetaDataIndexCount)
	{
		// WARNING ::: Dont put condition here for optimizaion , if Metaavailable !!!!
		// Meta may be added for other track . Also this is the method by which DrmManger knows Meta
		// is still available after refresh . Else it will flush out the Meta from DrmManager thinking
		// Source removed the source
		for (int idx = 0; idx < mDrmMetaDataIndexCount; idx++)
		{
			traceprintf("%s:%d:[%s] Setting  metadata for index %d/%d", __FUNCTION__, __LINE__,name, idx,mDrmMetaDataIndexCount);
			AveDrmManager::SetMetadata(context->aamp, &drmMetadataNode[idx],(int)type);
		}
	}
}

/***************************************************************************
* @fn InitiateDRMKeyAcquisition
* @brief Function to initiate key request for all Meta data
***************************************************************************/
void TrackState::InitiateDRMKeyAcquisition(int indexPosn)
{
	// WARNING :: Dont put optimization condition here to check if MetaAvailable or KeyAvailable.
	// This is the call by which DrmManager runs the loops to initiate key request for deferred

	// Initiate Key Request will happen after every refresh for all the Meta as there is no pre-refresh data stored to compare
	// Inside AveDrmManager, check is done if Key is already acquired or not . Also if any deferred request is needed or not
	// Second caller of this function is SetDrmContext,if Key is not acquired then for specific meta index key request is made
	//logprintf("%s:%d:[%s] mDrmMetaDataIndexCount %d ", __FUNCTION__, __LINE__,name, mDrmMetaDataIndexCount);
	DrmMetadataNode* drmMetadataNode = (DrmMetadataNode*)mDrmMetaDataIndex.ptr;
	long long currentTime = aamp_GetCurrentTimeMS();
	bool retStatus = true;
	// Function to initiate key request with DRM
	// if indexPosn == -1 , its required to check for all the Metadata stored in the list
	// 		this call will be made after playlist update .
	// if indexPosn != -1 , its required to initiate key request immediately for particular Meta
	if(mDrmMetaDataIndexCount > 0)
	{
		// if it is for Adobe DRM only , upfront Key request is done ,
		// If Clear , nothing to do
		if(indexPosn == -1)
		{
			for (int idx = 0; idx < mDrmMetaDataIndexCount; idx++)
			{
				// Request key for all Meta's received in playlist refresh .
				// Deferring logic is with DRM Manager ..lets make it little more intelligent instead of FragCollector
				// doing all checks n calculcation .
				// Every refresh of playlist , DRM Meta will calculate new refresh time and request. DRM Manager knows the
				// first request with deferred time , so it will request key when time comes.
				traceprintf("%s:%d:[%s]Request DRM Key for indexPosn[%d]",__FUNCTION__, __LINE__,name,idx);
				retStatus = AveDrmManager::AcquireKey(context->aamp, &drmMetadataNode[idx],(int)type);
				if(retStatus == false)
					break;
			}
		}
		else
		{
			// on an emergency may have to request Key for certain index only
			logprintf("%s:%d:[%s]Request DRM Key immediately for indexPosn[%d]",__FUNCTION__, __LINE__,name,indexPosn);
			retStatus = AveDrmManager::AcquireKey(context->aamp, &drmMetadataNode[indexPosn],(int)type,true);
		}

		if(retStatus == false)
		{
			// Something wrong , why should AveDrmManager return false when Key is requested,
			// May be Meta is not stored before requesting Key or Meta may not be available for ShaId looking for
			logprintf("%s:%d:[%s] Failure to Get Key ",__FUNCTION__, __LINE__,name);
			aamp->SendErrorEvent(AAMP_TUNE_INVALID_MANIFEST_FAILURE, NULL, true);
		}
	}
}

/***************************************************************************
* @fn SetDrmContext
* @brief Function to set DRM Context when KeyTag changes
* @return None
***************************************************************************/
void TrackState::SetDrmContext()
{
	// Set the appropriate DrmContext for Decryption
	// This function need to be called where KeyMethod != None is found after indexplaylist
	// or when new KeyMethod is found , None to AES or between AES with different Method
	// or between KeyMethond when IV or URL changes (for Vanilla AES)

	bool drmContextUpdated = false;
	DrmMetadataNode* drmMetadataIdx = (DrmMetadataNode*)mDrmMetaDataIndex.ptr;

	if(drmMetadataIdx)
	{
		logprintf("TrackState::[%s][%s] Enter mCMSha1Hash [%p] mDrmMetaDataIndexPosition %d", __FUNCTION__,name, mCMSha1Hash,
			mDrmMetaDataIndexPosition);
		// Get the DRM Instance based on current Sha1
		// a) If Multi Key involved mCMSha1Hash will have value ,based on which DRM Instance is picked
		// b) If Single Key invloved (no mCMSha1Hash), but with diff Meta for audio and video , based on track type appropriate DRM instance picked
		// c) If Single Key involved (no mCMSha1Hash) , audio/video having same meta, AveDrmManager will get the the only on Drm Instance
		mDrm = AveDrmManager::GetAveDrm(mCMSha1Hash,type);
		if(mDrm && mDrm->GetState() != DRMState::eDRM_KEY_ACQUIRED)
			{
				// Need of the hour ,initiate the key before the decrypt function is called
				logprintf("%s:%d:[%s] Initiating Key Request as Key is not available for index [%d]",__FUNCTION__,__LINE__,name,mDrmMetaDataIndexPosition);
				InitiateDRMKeyAcquisition(mDrmMetaDataIndexPosition);
			}
	}
	else
	{
#ifdef AAMP_VANILLA_AES_SUPPORT
		AAMPLOG_INFO("StreamAbstractionAAMP_HLS::%s:%d Get AesDec", __FUNCTION__, __LINE__);
		mDrm = AesDec::GetInstance();
#else
		logprintf("StreamAbstractionAAMP_HLS::%s:%d AAMP_VANILLA_AES_SUPPORT not defined", __FUNCTION__, __LINE__);
#endif
	}
	if(mDrm)
	{
		mDrm->SetDecryptInfo(aamp, &mDrmInfo);
	}
}

/***************************************************************************
* @fn GetNextLineStart
* @brief Function to get start of the next line
* @param[in] ptr buffer to do the operation
* @return start of the next line
***************************************************************************/
static char* GetNextLineStart(char* ptr)
{
	ptr = strchr(ptr, CHAR_LF);
	if( ptr )
	{
	    ptr++;
	}
	return ptr;
}

/***************************************************************************
* @fn FindLineLength
* @brief Function to get the length of line.
* @param[in] ptr start of line
* @return length of line
***************************************************************************/
static size_t FindLineLength(const char* ptr)
{
	size_t len;
        /*
        Lines in a Playlist file are terminated by either a single line feed
        character or a carriage return character followed by an line feed
        */
	const char * delim = strchr(ptr, CHAR_LF);
	if (delim)
	{
		len = delim-ptr;
		if (delim > ptr && delim[-1] == CHAR_CR)
		{
			len--;
		}
	}
	else
	{
		len = strlen(ptr);
	}
	return len;
}

/***************************************************************************
* @fn IndexPlaylist
* @brief Function to parse playlist 
*		 
* @return double total duration from playlist
***************************************************************************/
void TrackState::IndexPlaylist()
{
	double totalDuration = 0.0;
	traceprintf("%s:%d Enter ", __FUNCTION__, __LINE__);
	pthread_mutex_lock(&mPlaylistMutex);
	FlushIndex();
	mIndexingInProgress = true;
	if (playlist.ptr )
	{
		char *ptr;
		if(memcmp(playlist.ptr,"#EXTM3U",7)!=0)
		{
		    int tempDataLen = (MANIFEST_TEMP_DATA_LENGTH - 1);
		    char temp[MANIFEST_TEMP_DATA_LENGTH];
		    strncpy(temp, playlist.ptr, tempDataLen);
		    temp[tempDataLen] = 0x00;
		    logprintf("ERROR: Invalid Playlist URL:%s ", mPlaylistUrl.c_str());
		    logprintf("ERROR: Invalid Playlist DATA:%s ", temp);
		    aamp->SendErrorEvent(AAMP_TUNE_INVALID_MANIFEST_FAILURE);
		    mDuration = totalDuration;
		    pthread_cond_signal(&mPlaylistIndexed);
		    pthread_mutex_unlock(&mPlaylistMutex);
		    return;
		}
		DrmMetadataNode drmMetadataNode;
		IndexNode node;
		node.completionTimeSecondsFromStart = 0.0;
		node.pFragmentInfo = NULL;
		int drmMetadataIdx = -1;
		bool deferDrmTagPresent = false;
		const char* deferDrmVal = NULL;
		bool endOfTopTags = false;
		bool mediaSequence = false;
		const char* programDateTimeIdxOfFragment = NULL;
		bool discontinuity = false;
		ptr = GetNextLineStart(playlist.ptr);
		while (ptr)
		{
			if(startswith(&ptr,"#EXT"))
			{
				if (startswith(&ptr,"INF:"))
				{
					if (discontinuity)
					{
						logprintf("%s:%d #EXT-X-DISCONTINUITY in track[%d] indexCount %d periodPosition %f", __FUNCTION__, __LINE__, type, indexCount, totalDuration);
						DiscontinuityIndexNode discontinuityIndexNode;
						discontinuityIndexNode.fragmentIdx = indexCount;
						discontinuityIndexNode.position = totalDuration;
						discontinuityIndexNode.programDateTime = programDateTimeIdxOfFragment;
						discontinuityIndexNode.fragmentDuration = atof(ptr);
						aamp_AppendBytes(&mDiscontinuityIndex, &discontinuityIndexNode, sizeof(DiscontinuityIndexNode));
						mDiscontinuityIndexCount++;
						discontinuity = false;
					}
					programDateTimeIdxOfFragment = NULL;
					node.pFragmentInfo = ptr-8;//Point to beginning of #EXTINF
					indexCount++;
					totalDuration += atof(ptr);
					node.completionTimeSecondsFromStart = totalDuration;
					node.drmMetadataIdx = drmMetadataIdx;
					aamp_AppendBytes(&index, &node, sizeof(node));
				}
				else if(startswith(&ptr,"-X-MEDIA-SEQUENCE:"))
				{
					indexFirstMediaSequenceNumber = atoll(ptr);
					mediaSequence = true;
				}
				else if(startswith(&ptr,"-X-TARGETDURATION:"))
				{
					targetDurationSeconds = atof(ptr);
					AAMPLOG_INFO("aamp: EXT-X-TARGETDURATION = %f", targetDurationSeconds);
				}
				else if(startswith(&ptr,"-X-X1-LIN-CK:"))
				{
					// get the deferred drm key acquisition time
					mDeferredDrmKeyMaxTime = atoi(ptr);
					AAMPLOG_INFO("%s:%d: #EXT-X-LIN [%d]",__FUNCTION__, __LINE__, mDeferredDrmKeyMaxTime);
				}
				else if(startswith(&ptr,"-X-PLAYLIST-TYPE:"))
				{
					// EVENT or VOD (optional); VOD if playlist will never change
					if (startswith(&ptr, "VOD"))
					{
						logprintf("aamp: EXT-X-PLAYLIST-TYPE - VOD");
						mPlaylistType = ePLAYLISTTYPE_VOD;
					}
					else if (startswith(&ptr, "EVENT"))
					{
						logprintf("aamp: EXT-X-PLAYLIST-TYPE = EVENT");
						mPlaylistType = ePLAYLISTTYPE_EVENT;
					}
					else
					{
						aamp_Error("unknown PLAYLIST-TYPE");
					}
				}
				else if(startswith(&ptr,"-X-FAXS-CM:"))
				{
					size_t srcLen;
					traceprintf("aamp: #EXT-X-FAXS-CM:");
					srcLen = FindLineLength(ptr);
					unsigned char hash[SHA_DIGEST_LENGTH] = {0};
					drmMetadataNode.deferredInterval = mDeferredDrmKeyMaxTime;
					drmMetadataNode.drmKeyReqTime = 0;
					drmMetadataNode.metaData.metadataPtr =  base64_Decode(ptr, &drmMetadataNode.metaData.metadataSize, srcLen);
					SHA1(drmMetadataNode.metaData.metadataPtr, drmMetadataNode.metaData.metadataSize, hash);
					drmMetadataNode.sha1Hash = base16_Encode(hash, SHA_DIGEST_LENGTH);
	#ifdef TRACE
					logprintf("%s:%d [%s] drmMetadataNode[%d].sha1Hash -- ", __FUNCTION__, __LINE__, name, mDrmMetaDataIndexCount);
					for (int i = 0; i < DRM_SHA1_HASH_LEN; i++)
					{
						printf("%c", drmMetadataNode.sha1Hash[i]);
					}
					printf("\n");
	#endif
					aamp_AppendBytes(&mDrmMetaDataIndex, &drmMetadataNode, sizeof(drmMetadataNode));
					traceprintf("%s:%d mDrmMetaDataIndex.ptr %p", __FUNCTION__, __LINE__, mDrmMetaDataIndex.ptr);
					mDrmMetaDataIndexCount++;
				}
				else if(startswith(&ptr,"-X-DISCONTINUITY-SEQUENCE"))
				{
					// ignore sequence
				}
				else if(startswith(&ptr,"-X-DISCONTINUITY"))
				{
					discontinuity = true;
				}
				else if (startswith(&ptr, "-X-PROGRAM-DATE-TIME:"))
				{
					programDateTimeIdxOfFragment = ptr;
					traceprintf("Got EXT-X-PROGRAM-DATE-TIME: %.*s ", 30, programDateTimeIdxOfFragment);
				}
				else if (startswith(&ptr, "-X-KEY:"))
				{
					size_t len;
					traceprintf("aamp: EXT-X-KEY");
					len = FindLineLength(ptr);
					char* key =(char*) malloc (len+1);
					memcpy(key,ptr,len);
					key[len]='\0';

					// Need to store the Key tag to a list . Needs listed below
					// a) When a new Meta is added , its hash need to be compared
					//with available keytags to determine if its a deferred KeyAcquisition or not(VSS)
					// b) If there is a stream with varying IV in keytag with single Meta,
					// check if during trickplay drmInfo is considered.
					KeyTagStruct keyinfo;
					keyinfo.mKeyStartDuration = totalDuration;
					keyinfo.mKeyTagStr.resize(len);
					memcpy((char*)keyinfo.mKeyTagStr.data(),key,len);

					ParseAttrList(key, ParseKeyAttributeCallback, this);
					//Each fragment should store the corresponding keytag indx to decrypt, MetaIdx may work with
					// adobe mapping , then if or any attribute of Key tag is different ?
					// At present , second Key parsing is done inside GetNextFragmentUriFromPlaylist(that saved)
					//Need keytag idx to pick the corresponding keytag and get drmInfo,so that second parsing can be removed
					//drmMetadataIdx = mDrmMetaDataIndexPosition;
					drmMetadataIdx = mDrmKeyTagCount;
					if(!fragmentEncrypted || mDrmMethod == eDRM_KEY_METHOD_SAMPLE_AES_CTR)
					{
						drmMetadataIdx = -1;
						traceprintf("%s:%d Not encrypted - fragmentEncrypted %d mCMSha1Hash %p mDrmMethod %d", __FUNCTION__, __LINE__, fragmentEncrypted, mCMSha1Hash, mDrmMethod);
					}

					// mCMSha1Hash is populated after ParseAttrList , hence added here
					if(mCMSha1Hash)
					{
						keyinfo.mShaID.resize(DRM_SHA1_HASH_LEN);
						memcpy((char*)keyinfo.mShaID.data(), mCMSha1Hash, DRM_SHA1_HASH_LEN);
					}
					mKeyHashTable.push_back(keyinfo);
					mKeyTagChanged = false;

					free (key);
					mDrmKeyTagCount++;
				}
				else if(startswith(&ptr,"-X-MAP:"))
				{
					if (mCheckForInitialFragEnc)
					{
						AAMPLOG_TRACE("%s:%d fragmentEncrypted-%d drmMethod-%d and ptr - %s", __FUNCTION__, __LINE__, fragmentEncrypted, mDrmMethod, ptr);
						// Map tag present indicates ISOBMFF fragments. We need to store an encrypted fragment's init header
						// Ensure order of tags 1. EXT-X-KEY, 2. EXT-X-MAP
						if (fragmentEncrypted && mDrmMethod == eDRM_KEY_METHOD_SAMPLE_AES_CTR && mFirstEncInitFragmentInfo == NULL)
						{
							AAMPLOG_TRACE("%s:%d mFirstEncInitFragmentInfo - %s", __FUNCTION__, __LINE__, ptr);
							mFirstEncInitFragmentInfo = ptr;
						}
					}
				}
				else if (startswith(&ptr, "-X-START:"))
				{
					// X-Start can have two attributes . Time-Offset & Precise .
					// check if App has not configured any liveoffset
					if(!aamp->mNewLiveOffsetflag)
					{
					 	double offsetval = ParseXStartTimeOffset(ptr);
		                             	if(!aamp->IsLiveAdjustRequired())
         					{
                                                      	// if aamp cfg offset is not set or App has not set the value  , then configure
                                                       	if(gpGlobalConfig->cdvrliveOffset == -1)
                                                                SetXStartTimeOffset(offsetval);
                                                }
                                                else
                                                {
                                                        // if aamp cfg offset is not set or App has not set the value , then configure
                                                        if(gpGlobalConfig->liveOffset == -1)
								SetXStartTimeOffset(offsetval);
                                                }
                                        }
				}
				else if (startswith(&ptr,"-X-ENDLIST"))
				{
					// ENDLIST found .Check playlist tag with vod was missing or not.If playlist still undefined
					// mark it as VOD
					if (IsLive())
					{
						if (mPlaylistType == ePLAYLISTTYPE_UNDEFINED)
						{
							logprintf("aamp: Found EXT-X-ENDLIST without EXT-X-PLAYLIST-TYPE");
						}
						else
						{
							logprintf("aamp: Found EXT-X-ENDLIST with ePLAYLISTTYPE_EVENT");
						}
						//required to avoid live adjust kicking in
						logprintf("aamp: Changing playlist type to ePLAYLISTTYPE_VOD as ENDLIST tag present");
						mPlaylistType = ePLAYLISTTYPE_VOD;
					}
				}
			}
			ptr=GetNextLineStart(ptr);
		}

		if (mDrmMetaDataIndexCount > 1)
		{
			logprintf("%s:%d[%d] Indexed %d drm metadata", __FUNCTION__, __LINE__,type, mDrmMetaDataIndexCount);
		}

		// DELIA-33434
		// Update DRM Manager for stored indexes so that it can be removed after playlist update
		// Update is required only for multi key stream, where Sha1 is set ,for single key stream,
		// SetMetadata is not called across playlist update , hence Update is not needed
		// Have to check for normal playback only. SAP Audio in VSS sometimes having total different
		// Meta set from video.So when iframe manifest is parsed, audio Meta shouldnt be cleared.
		// Trickplay is for short duration , when back to normal rate,it will flush iframe specific Metas 
		if(mCMSha1Hash && context->rate == AAMP_NORMAL_PLAY_RATE)
		{
			AveDrmManager::UpdateBeforeIndexList(name,(int)type);
		}

		if(mediaSequence==false)
		{ // for Sling content
			AAMPLOG_INFO("warning: no EXT-X-MEDIA-SEQUENCE tag");
			ptr = playlist.ptr;
			indexFirstMediaSequenceNumber = 0;
		}
		// DELIA-35008 When setting live status to stream , check the playlist type of both video/audio(demuxed)
		aamp->SetIsLive(context->IsLive());
		if(!IsLive())
		{
			aamp->getAampCacheHandler()->InsertToPlaylistCache(mPlaylistUrl, &playlist, mEffectiveUrl,IsLive(),(MediaType)type);
		}
		if(eTRACK_VIDEO == type)
		{
			aamp->UpdateDuration(totalDuration);
		}

	}
#ifdef TRACE
	DumpIndex(this);
#endif

	if(mDeferredDrmKeyMaxTime != 0)
	{
		// Special stream with Deferred DRM Key Acquisition required
		ComputeDeferredKeyRequestTime();
	}
	// No condition checks for call . All checks n balances inside the module
	// which is been called.
	// Store the all the Metadata received from playlist indexing .
	// IF already stored , AveDrmManager will ignore it
	// ProcessDrmMetadata -> to be called only from one place , after playlist indexing. Not to call from other places
	aamp->profiler.ProfileBegin(PROFILE_BUCKET_LA_TOTAL);
	ProcessDrmMetadata();
	// Initiating key request for Meta present.If already key received ,call will be ignored.
	InitiateDRMKeyAcquisition();
	// default MetaIndex is 0 , for single Meta . If Multi Meta is there ,then Hash is the criteria
	// for selection
	mDrmMetaDataIndexPosition = 0;

	if (mDrmKeyTagCount > 0)
	{
		if (mDrmMetaDataIndexCount > 0)
		{
			aamp->setCurrentDrm(eDRM_Adobe_Access);
		}
		else
		{
			aamp->setCurrentDrm(eDRM_Vanilla_AES);
		}
	}
	firstIndexDone = true;
	mIndexingInProgress = false;
	traceprintf("%s:%d Exit indexCount %d mDrmMetaDataIndexCount %d", __FUNCTION__, __LINE__, indexCount, mDrmMetaDataIndexCount);
	mDuration = totalDuration;
	// DELIA-33434
	// Update is required only for multi key stream, where Sha1 is set ,for single key stream,
	// SetMetadata is not called across playlist update hence flush is not needed
	if(mCMSha1Hash && context->rate == AAMP_NORMAL_PLAY_RATE)
	{
		AveDrmManager::FlushAfterIndexList(name,(int)type);
	}
	pthread_cond_signal(&mPlaylistIndexed);
	pthread_mutex_unlock(&mPlaylistMutex);
}

#ifdef AAMP_HARVEST_SUPPORT_ENABLED
/***************************************************************************
* @fn HarvestFile
* @brief Function to harvest stream contents locally for debugging 
*		 
* @param url[in] url string	
* @param buffer[in] data content
* @param isFragment[in] flag indicating fragment or not
* @param prefix[in] prefix string to add to file name 
* @return void
***************************************************************************/
void StreamAbstractionAAMP_HLS::HarvestFile(std::string url, GrowableBuffer* buffer, bool isFragment, const char* prefix)
{
	if (aamp->HarvestFragments(isFragment))
	{
		logprintf("aamp: hls Harvest %s len %d", url.c_str(), (int)buffer->len);
		std::string path = "/media/tsb/"; // SD card on xi3v2
		std::size_t pos = url.rfind('/');
		if (pos != std::string::npos)
		{
			std::string prefix = url.substr(pos+1);
			path += prefix;

			std::ofstream f(path, std::ofstream::binary);
			if (f.good())
			{
				f.write(buffer->ptr, buffer->len);
				logprintf("aamp: hls -harvest written %s buffer.len %d", path.c_str(), (int)buffer->len);
				f.close();
			}
			else
			{
				logprintf("aamp: hls -harvest file open failed %s len %d", path.c_str(), (int)buffer->len);
			}
		}
	}
}
#endif
/***************************************************************************
* @fn ABRProfileChanged
* @brief Function to handle Profile change after ABR  
*		 
* @return void
***************************************************************************/

void TrackState::ABRProfileChanged()
{
	// If not live, reset play position since sequence number doesn't ensure the fragments
	// from multiple streams are in sync
	traceprintf("%s:%d playlistPosition %f", __FUNCTION__,__LINE__, playlistPosition);
	aamp_ResolveURL(mPlaylistUrl, aamp->GetManifestUrl(), context->GetPlaylistURI(type));
	pthread_mutex_lock(&mutex);
	//playlistPosition reset will be done by RefreshPlaylist once playlist downloaded successfully
	//refreshPlaylist is used to reset the profile index if playlist download fails! Be careful with it.
	//Video profile change will definitely require new init headers
	mInjectInitFragment = true;
	refreshPlaylist = true;
	/*For some VOD assets, different video profiles have different DRM meta-data.*/
	mForceProcessDrmMetadata = true;
	pthread_mutex_unlock(&mutex);
}
/***************************************************************************
* @fn RefreshPlaylist
* @brief Function to redownload playlist after refresh interval .
*		 
* @return void
***************************************************************************/
void TrackState::RefreshPlaylist(void)
{
	// use current, not next position to synchronize and detect culling
	// important, as 'next position' typically isn't present in both
	// the before and after playlist
	long long commonPlayPosition = nextMediaSequenceNumber - 1;
	double prevSecondsBeforePlayPoint = GetCompletionTimeForFragment(this, commonPlayPosition);
	GrowableBuffer tempBuff;
	long http_error = 0;

	// note: this used to be updated only upon succesful playlist download
	// this can lead to back-to-back playlist download retries
	lastPlaylistDownloadTimeMS = aamp_GetCurrentTimeMS();


	if(playlist.ptr)
	{
		tempBuff.len = playlist.len;
		tempBuff.avail = playlist.avail;
		tempBuff.ptr = playlist.ptr;
		memset(&playlist, 0, sizeof(playlist));
	}
	else
	{
		memset(&tempBuff, 0, sizeof(tempBuff));
	}

	// DELIA-34993 -> Refresh playlist gets called on ABR profile change . For VOD if already present , pull from cache. 
	bool bCacheRead = false;
	if (!IsLive())
	{
		bCacheRead = aamp->getAampCacheHandler()->RetrieveFromPlaylistCache(mPlaylistUrl, &playlist, mEffectiveUrl);
	}
	// failed to read from cache , then download it 
	if(!bCacheRead)
	{
		int iCurrentRate = aamp->rate; //  Store it as back up, As sometimes by the time File is downloaded, rate might have changed due to user initiated Trick-Play
		aamp->GetFile(mPlaylistUrl, &playlist, mEffectiveUrl, &http_error, NULL, type, true, eMEDIATYPE_MANIFEST);
		//update videoend info
		MediaType actualType = eMEDIATYPE_PLAYLIST_VIDEO ;
		if(IS_FOR_IFRAME(iCurrentRate,type))
		{
			actualType = eMEDIATYPE_PLAYLIST_IFRAME;
		}
		else if (type == eTRACK_AUDIO )
		{
			actualType = eMEDIATYPE_PLAYLIST_AUDIO ;
		}

		aamp->UpdateVideoEndMetrics( actualType,
								(this->GetCurrentBandWidth()*8),
								http_error,mEffectiveUrl);
	}
	if (playlist.len)
	{ // download successful
		//lastPlaylistDownloadTimeMS = aamp_GetCurrentTimeMS();
		if (context->mNetworkDownDetected)
		{
			context->mNetworkDownDetected = false;
		}
		aamp_Free(&tempBuff.ptr);
		aamp_AppendNulTerminator(&playlist); // hack: make safe for cstring operationsaamp_AppendNulTerminator(&this->mainManifest); // make safe for cstring operations
#ifdef TRACE
		if (gpGlobalConfig->logging.trace)
		{
			printf("***New Playlist:**************\n\n%s\n*************\n", playlist.ptr);
		}
#endif
		IndexPlaylist();

		// Update culled seconds if playlist download was successful
		// DELIA-40121: We need culledSeconds to find the timedMetadata position in playlist
		// culledSeconds and FindTimedMetadata have been moved up here, because FindMediaForSequenceNumber
		// uses mystrpbrk internally which modifies line terminators in playlist.ptr and results in 
		// FindTimedMetadata failing to parse playlist
		if (IsLive())
		{
			double newSecondsBeforePlayPoint = GetCompletionTimeForFragment(this, commonPlayPosition);
			double culled = prevSecondsBeforePlayPoint - newSecondsBeforePlayPoint;
			if (culled > 0)
			{
				mCulledSeconds += culled;
				if (eTRACK_VIDEO == type)
				{
					aamp->UpdateCullingState(culled); // report amount of content that was implicitly culled since last playlist download
				}
			}
		}

		//Parse for timed metadata in the updated playlist
		FindTimedMetadata();

		if( mDuration > 0.0f )
		{
#ifdef AAMP_HARVEST_SUPPORT_ENABLED
			const char* prefix = (type == eTRACK_SUBTITLE)?"sub-":(type == eTRACK_AUDIO)?"aud-":(context->trickplayMode)?"ifr-":"vid-";
			context->HarvestFile(mPlaylistUrl, &playlist, false, prefix);
#endif
			if (IsLive())
			{
				fragmentURI = FindMediaForSequenceNumber();
			}
			else
			{
				fragmentURI = playlist.ptr;
				playlistPosition = -1;
			}
			manifestDLFailCount = 0;
		}
	}
	else
	{
		//Restore playlist in case of failure
		if (tempBuff.ptr)
		{
			playlist.ptr = tempBuff.ptr;
			playlist.len = tempBuff.len;
			playlist.avail = tempBuff.avail;
			//Refresh happened due to ABR switching, we need to reset the profileIndex
			//so that ABR can be attempted later
			if (refreshPlaylist)
			{
				context->currentProfileIndex = context->lastSelectedProfileIndex;
			}
		}

		if (aamp->DownloadsAreEnabled())
		{
			if (CURLE_OPERATION_TIMEDOUT == http_error || CURLE_COULDNT_CONNECT == http_error)
			{
				context->mNetworkDownDetected = true;
				logprintf("%s:%d Ignore curl timeout", __FUNCTION__, __LINE__);
				return;
			}
			manifestDLFailCount++;

			if(fragmentURI == NULL && (manifestDLFailCount > MAX_MANIFEST_DOWNLOAD_RETRY))//No more fragments to download
			{
				aamp->SendDownloadErrorEvent(AAMP_TUNE_MANIFEST_REQ_FAILED, http_error);
				return;
			}
		}
	}
}
/***************************************************************************
* @fn GetPlaylistURI
* @brief Function to get playlist URI based on media selection 
*		 
* @param trackType[in] Track type 
* @param format[in] stream output type
* @return string playlist URI 
***************************************************************************/
const char *StreamAbstractionAAMP_HLS::GetPlaylistURI(TrackType trackType, StreamOutputFormat* format)
{
	const char *playlistURI = NULL;
	const char* group = NULL;
	HlsStreamInfo* streamInfo = NULL;

	switch (trackType)
	{
	case eTRACK_VIDEO:
		playlistURI = this->streamInfo[this->currentProfileIndex].uri;
		if (format)
		{
			*format = FORMAT_MPEGTS;
		}
		break;

	case eTRACK_AUDIO:
		assert(GetProfileCount() > this->currentProfileIndex);
		streamInfo = &this->streamInfo[this->currentProfileIndex];
		group = streamInfo->audio;
		if (group)
		{
			logprintf("GetPlaylistURI : AudioTrack: group %s, aamp->language %s", group, aamp->language);
			bool foundAudio = false;
			for(int langChecks = aamp->language[0] ? 2 : 1; langChecks > 0 && !foundAudio; --langChecks)
			{
				for (int i = 0; i < mMediaCount; i++)
				{
	#ifdef TRACE
					logprintf("GetPlaylistURI : AudioTrack: this->mediaInfo[%d].group_id %s", i,
						this->mediaInfo[i].group_id);
	#endif
					if (this->mediaInfo[i].group_id && !strcmp(group, this->mediaInfo[i].group_id))
					{
//	#ifdef TRACE
						logprintf("GetPlaylistURI checking if preferred language '%s' matches media[%d] language '%s'", aamp->language, i, this->mediaInfo[i].language);
//	#endif
						if ((aamp->language[0] && this->mediaInfo[i].language && strncmp(aamp->language, this->mediaInfo[i].language, MAX_LANGUAGE_TAG_LENGTH)==0) || (langChecks == 1 && (this->mediaInfo[i].isDefault || this->mediaInfo[i].autoselect)))
						{
							foundAudio = true;
							if(langChecks == 1)
							{
								//save what language we have selected, defaulting to english
								logprintf("%s updating aamp->language from %s to %s mediaInfo[i].language %s", __FUNCTION__, aamp->language, this->mediaInfo[i].language ? this->mediaInfo[i].language : "en", mediaInfo[i].language);
								aamp->UpdateAudioLanguageSelection((this->mediaInfo[i].language ? this->mediaInfo[i].language : "en"));
								if(!this->mediaInfo[i].language)
								{
									logprintf("GetPlaylistURI : language not found. Instead, select default of %s", aamp->language);
								}
							}
							playlistURI = this->mediaInfo[i].uri;
							logprintf("GetPlaylistURI language found uri %s", playlistURI);
							if (playlistURI)
							{
								logprintf("GetPlaylistURI : AudioTrack: playlistURI %s", playlistURI);
							}
							else
							{
								logprintf("GetPlaylistURI : AudioTrack: NULL playlistURI. this->mediaInfo[i].isDefault %d", this->mediaInfo[i].isDefault);
							}
							if (format)
							{
								*format = FORMAT_NONE;
								if (this->mediaInfo[i].uri && streamInfo->codecs)
								{
	#ifdef TRACE
									logprintf("GetPlaylistURI : AudioTrack: streamInfo->codec %s", streamInfo->codecs);
	#endif
									for (int j = 0; j < AAMP_AUDIO_FORMAT_MAP_LEN; j++)
									{
										if (strstr(streamInfo->codecs, audioFormatMap[j].codec))
										{
											*format = audioFormatMap[j].format;
											logprintf("GetPlaylistURI : AudioTrack: Audio format is %d [%s]",
												audioFormatMap[j].format, audioFormatMap[j].codec);
											break;
										}
									}
								}
							}
							break;
						}
					}
				}
			}
		}
		else if (!trickplayMode)
		{
			logprintf("%s updating aamp->language from %s to \"en\"", __FUNCTION__, aamp->language);
			//save that we are using english by default
			aamp->UpdateAudioLanguageSelection("en");
		}
		break;
	case eTRACK_SUBTITLE:
		{
			streamInfo = &this->streamInfo[this->currentProfileIndex];
			int mediaInfoIndex = -1;
			group = streamInfo->subtitles;
			if (group)
			{
				logprintf("StreamAbstractionAAMP_HLS::%s():%d Subtitle Track: group %s, aamp->mSubLanguage %s", __FUNCTION__, __LINE__, group, aamp->mSubLanguage);
				for (int i = 0; i < mMediaCount; i++)
				{
					if (mediaInfo[i].group_id && !strncmp(mediaInfo[i].group_id, group, strlen(group)))
					{
						if (aamp->mSubLanguage[0])
						{
							if (strncmp(aamp->mSubLanguage, mediaInfo[i].language, MAX_LANGUAGE_TAG_LENGTH) == 0)
							{
								//Found preferred language subtitle
								mediaInfoIndex = i;
								break;
							}
						}
					}
				}
				if (mediaInfoIndex != -1)
				{
					playlistURI = mediaInfo[mediaInfoIndex].uri;
					aamp->UpdateSubtitleLanguageSelection(mediaInfo[mediaInfoIndex].language);
					*format = (mediaInfo[mediaInfoIndex].type == eMEDIATYPE_SUBTITLE) ? FORMAT_SUBTITLE_WEBVTT : FORMAT_NONE;
					logprintf("StreamAbstractionAAMP_HLS::%s():%d subtitle found language %s, uri %s and format %d", __FUNCTION__, __LINE__, mediaInfo[mediaInfoIndex].language, playlistURI, *format);
				}
				else
				{
					logprintf("StreamAbstractionAAMP_HLS::%s():%d Couldn't find subtitle URI for preferred language: %s", __FUNCTION__, __LINE__, aamp->mSubLanguage);
					*format = FORMAT_NONE;
				}
			}
		}
		break;
	}
	return playlistURI;
}

/***************************************************************************
* @fn GetFormatFromFragmentExtension
* @brief Function to get media format based on fragment extension
*		 
* @param trackState[in] TrackStatr structure pointer
* @return StreamOutputFormat stream format
***************************************************************************/
static StreamOutputFormat GetFormatFromFragmentExtension(TrackState *trackState)
{
	StreamOutputFormat format = FORMAT_INVALID;
	std::istringstream playlistStream(trackState->playlist.ptr);
	for (std::string line; std::getline(playlistStream, line); )
	{
		if(!line.empty() && (line.c_str()[0] != '#'))
		{
			while(isspace(line.back()))
			{
				line.pop_back();
				if (line.empty())
				{
				    break;
				}
			}
			if (line.empty())
			{
			    continue;
			}
			traceprintf("%s:%d line === %s ====", __FUNCTION__, __LINE__, line.c_str());
			size_t end = line.find("?");
			if (end != std::string::npos)
			{
				line = line.substr(0, end);
			}
			size_t extenstionStart = line.find_last_of('.');
			if (extenstionStart != std::string::npos)
			{
				std::string extension = line.substr(extenstionStart);
				traceprintf("%s:%d extension %s", __FUNCTION__, __LINE__, extension.c_str());
				if (0 == extension.compare(".ts"))
				{
					logprintf("%s:%d fragment extension %s - FORMAT_MPEGTS", __FUNCTION__, __LINE__, extension.c_str());
					format = FORMAT_MPEGTS;
				}
				else if (0 == extension.compare(".mp4") || 0 == extension.compare(".m4s") || 0 == extension.compare(".cmfv"))
				{
					logprintf("%s:%d fragment extension %s - FORMAT_ISO_BMFF", __FUNCTION__, __LINE__, extension.c_str());
					format = FORMAT_ISO_BMFF;
				}
				else if (0 == extension.compare(".aac"))
				{
					logprintf("%s:%d fragment extension %s - FORMAT_AUDIO_ES_AAC", __FUNCTION__, __LINE__, extension.c_str());
					format = FORMAT_AUDIO_ES_AAC;
				}
				else if (0 == extension.compare(".vtt"))
				{
					logprintf("%s:%d fragment extension %s - FORMAT_SUBTITLE_WEBVTT", __FUNCTION__, __LINE__, extension.c_str());
					format = FORMAT_SUBTITLE_WEBVTT;
				}
				else
				{
					logprintf("%s:%d Not TS or MP4 extension, probably ES. fragment extension %s len %d", __FUNCTION__, __LINE__, extension.c_str(), strlen(extension.c_str()));
				}
			}
			else
			{
				logprintf("%s:%d Could not find extension from line %s", __FUNCTION__, __LINE__, line.c_str());
			}
			break;
		}
	}
	return format;
}

/***************************************************************************
* @fn IsLive
* @brief Function to check if both tracks in demuxed HLS are in live mode
*
* @return True if both or any track in live mode
***************************************************************************/
bool StreamAbstractionAAMP_HLS::IsLive()
{
	// Check for both the tracks if its in Live state
	// In Demuxed content , Hot CDVR playlist update for audio n video happens at a small time delta
	// To avoid missing contents ,until both tracks are not moved to VOD , stream has to be in Live mode
	bool retValIsLive = false;
	for (int iTrack = 0; iTrack < AAMP_TRACK_COUNT; iTrack++)
	{
		TrackState *track = trackState[iTrack];
		if(track && track->Enabled())
		{
			retValIsLive |= track->IsLive();	
		}
	}
	return retValIsLive;
}

/***************************************************************************
* @fn CheckDiscontinuityAroundPlaytarget
* @brief Function to update play target based on audio video exact discontinuity positions.
*
* @return void
***************************************************************************/
void StreamAbstractionAAMP_HLS::CheckDiscontinuityAroundPlaytarget(void)
{
	TrackState *audio = trackState[eMEDIATYPE_AUDIO];
	TrackState *video = trackState[eMEDIATYPE_VIDEO];
	DiscontinuityIndexNode* videoDiscontinuityIndex = (DiscontinuityIndexNode*) video->mDiscontinuityIndex.ptr;
	DiscontinuityIndexNode* audioDiscontinuityIndex = (DiscontinuityIndexNode*) audio->mDiscontinuityIndex.ptr;

	for (int i = 0; i < video->mDiscontinuityIndexCount; i++)
	{
		if ((int)videoDiscontinuityIndex[i].position == video->playTarget)
		{
			if (videoDiscontinuityIndex[i].position < audioDiscontinuityIndex[i].position)
			{
				AAMPLOG_WARN("%s:%d video->playTarget %f -> %f audio->playTarget %f -> %f", __FUNCTION__, __LINE__,
								video->playTarget, videoDiscontinuityIndex[i].position, audio->playTarget, audioDiscontinuityIndex[i].position);
				video->playTarget = videoDiscontinuityIndex[i].position;
				audio->playTarget = audioDiscontinuityIndex[i].position;
			}
			else
			{
				AAMPLOG_WARN("%s:%d video->playTarget %f -> %d audio->playTarget %f -> %d", __FUNCTION__, __LINE__,
								video->playTarget, (int)audioDiscontinuityIndex[i].position, audio->playTarget, (int)audioDiscontinuityIndex[i].position);
				video->playTarget = audio->playTarget = (int)audioDiscontinuityIndex[i].position;
			}

			break;
		}
	}
}


/***************************************************************************
* @fn SyncTracksForDiscontinuity
* @brief Function to synchronize time between audio & video for VOD stream
*
* @return eAAMPSTATUS_OK on success
***************************************************************************/
AAMPStatusType StreamAbstractionAAMP_HLS::SyncTracksForDiscontinuity()
{
	TrackState *audio = trackState[eMEDIATYPE_AUDIO];
	TrackState *video = trackState[eMEDIATYPE_VIDEO];
	TrackState *subtitle = trackState[eMEDIATYPE_SUBTITLE];
	AAMPStatusType retVal = eAAMPSTATUS_GENERIC_ERROR;

	if (video->playTarget !=0)
	{
		/*If video playTarget is just before a discontinuity, move playTarget to the discontinuity position*/
		DiscontinuityIndexNode* videoDiscontinuityIndex = (DiscontinuityIndexNode*) video->mDiscontinuityIndex.ptr;
		DiscontinuityIndexNode* audioDiscontinuityIndex = (DiscontinuityIndexNode*) audio->mDiscontinuityIndex.ptr;

		for (int i = 0; i < video->mDiscontinuityIndexCount; i++)
		{
			double diff = videoDiscontinuityIndex[i].position - video->playTarget;

			if (diff == 0)
			{
				AAMPLOG_WARN ("%s:%d video track - diff = 0; breaking the loop; first discontinuity before the content/ad is accounted", __FUNCTION__, __LINE__);
				break;
			}

			if (diff < 0)
			{
				if (-(diff) < videoDiscontinuityIndex[i].fragmentDuration)
				{
					logprintf("%s:%d video track - playTarget [%f]->[%f] targetDurationSeconds %f",
							__FUNCTION__, __LINE__, video->playTarget, videoDiscontinuityIndex[i].position,
							video->targetDurationSeconds);

					video->playTarget = videoDiscontinuityIndex[i].position;

					if(i < audio->GetNumberOfPeriods())
					{
						logprintf("%s:%d audio track - playTarget [%f]->[%f] targetDurationSeconds %f",
							__FUNCTION__, __LINE__, audio->playTarget, audioDiscontinuityIndex[i].position,
							audio->targetDurationSeconds);

						audio->playTarget = audioDiscontinuityIndex[i].position;
					}

					break;
				}
			}
			else
			{
				CheckDiscontinuityAroundPlaytarget();
				break;
			}
		}

		if (audio->GetNumberOfPeriods() != video->GetNumberOfPeriods())
		{
			AAMPLOG_WARN("%s:%d WARNING audio's number of period %d video number of period %d\n", __FUNCTION__, __LINE__, audio->GetNumberOfPeriods(), video->GetNumberOfPeriods());
		}

		// DELIA-39083
		// If there is a mismatch in Number of Periods between audio and video , log a Warning but continue the period/discontinuity matching
		// for audio. From the stream analysis its observed
		//		a) An additional discontinuity is added at the end of the video playlist for one fragment ( <1sec duration)
		//		b) By avoiding the strict checking , no issue will happen for playback
		// But if delta is more than 1 , then it will impact playback .
		{
			int periodIdx;
			double offsetFromPeriod;
			double audioOffsetFromPeriod;
			video->GetNextFragmentPeriodInfo (periodIdx, offsetFromPeriod);

			if(-1 != periodIdx)
			{
				double audioPeriodStart = audio->GetPeriodStartPosition(periodIdx);
				double videoPeriodStart = video->GetPeriodStartPosition(periodIdx);

				audio->GetNextFragmentPeriodInfo (periodIdx, audioOffsetFromPeriod);

				AAMPLOG_WARN("%s:%d video periodIdx %d video-offsetFromPeriod %f videoPeriodStart %f audio-offsetFromPeriod %f audioPeriodStart %f",
								__FUNCTION__, __LINE__, periodIdx, offsetFromPeriod, videoPeriodStart, audioOffsetFromPeriod, audioPeriodStart);

				if (0 != audioPeriodStart )
				{
					audio->playTarget = audioPeriodStart + audioOffsetFromPeriod;
					video->playTarget = videoPeriodStart + offsetFromPeriod;

					seekPosition = video->playTarget;

					AAMPLOG_WARN("%s:%d seek_pos_seconds changed to %f based on video playTarget", __FUNCTION__, __LINE__, seekPosition);

					retVal = eAAMPSTATUS_OK;
				}
				else
				{
					logprintf("%s:%d audioDiscontinuityOffset 0", __FUNCTION__, __LINE__);
				}
			}
		}

		if (subtitle->enabled)
		{
			if (audio->GetNumberOfPeriods() == subtitle->GetNumberOfPeriods())
			{
				int periodIdx;
				double offsetFromPeriod;
				audio->GetNextFragmentPeriodInfo(periodIdx, offsetFromPeriod);
				if(-1 != periodIdx)
				{
					logprintf("%s:%d audio periodIdx %d offsetFromPeriod %f", __FUNCTION__, __LINE__, periodIdx, offsetFromPeriod);
					double subtitlePeriodStart = subtitle->GetPeriodStartPosition(periodIdx);
					if (0 != subtitlePeriodStart)
					{
						subtitle->playTarget = subtitlePeriodStart + offsetFromPeriod;
					}
					else
					{
						logprintf("%s:%d subtitleDiscontinuityOffset 0", __FUNCTION__, __LINE__);
					}
				}
			}
			else
			{
				logprintf("%s:%d WARNING audio's number of period %d subtitle number of period %d", __FUNCTION__, __LINE__, audio->GetNumberOfPeriods(), subtitle->GetNumberOfPeriods());
			}
			logprintf("%s Exit : audio track start %f, vid track start %f sub track start %f", __FUNCTION__, audio->playTarget, video->playTarget, subtitle->playTarget);
		}
		else
		{
			logprintf("%s Exit : audio track start %f, vid track start %f", __FUNCTION__, audio->playTarget, video->playTarget );
		}
	}

	return retVal;
}

/***************************************************************************
* @fn SyncTracks
* @brief Function to synchronize time between A/V for Live/Event assets
* @param useProgramDateTimeIfAvailable use program date time tag to sync if available
* @return eAAMPSTATUS_OK on success
***************************************************************************/
AAMPStatusType StreamAbstractionAAMP_HLS::SyncTracks(bool useProgramDateTimeIfAvailable)
{
	AAMPStatusType retval = eAAMPSTATUS_OK;
	bool startTimeAvailable = true;
	bool syncedUsingSeqNum = false;
	long long mediaSequenceNumber[AAMP_TRACK_COUNT];
	TrackState *audio = trackState[eMEDIATYPE_AUDIO];
	TrackState *video = trackState[eMEDIATYPE_VIDEO];
	TrackState *subtitle = trackState[eMEDIATYPE_SUBTITLE];
	double diffBetweenStartTimes = 0;
	for(int i = 0; i<AAMP_TRACK_COUNT; i++)
	{
		TrackState *ts = trackState[i];
		if (ts->enabled)
		{
			ts->fragmentURI = trackState[i]->GetNextFragmentUriFromPlaylist(true); //To parse track playlist
			/*Update playTarget to playlistPostion to correct the seek position to start of a fragment*/
			ts->playTarget = ts->playlistPosition;
			logprintf("syncTracks loop : track[%d] pos %f start %f frag-duration %f trackState->fragmentURI %s ts->nextMediaSequenceNumber %lld", i, ts->playlistPosition, ts->playTarget, ts->fragmentDurationSeconds, ts->fragmentURI, ts->nextMediaSequenceNumber);
			if (ts->startTimeForPlaylistSync.tv_sec == 0)
			{
				logprintf("startTime not available for track %d", i);
				startTimeAvailable = false;
			}
			mediaSequenceNumber[i] = ts->nextMediaSequenceNumber - 1;
		}
	}

	if (startTimeAvailable)
	{
		//Logging irregularities in the playlist for debugging purposes
		diffBetweenStartTimes = trackState[eMEDIATYPE_AUDIO]->startTimeForPlaylistSync.tv_sec - trackState[eMEDIATYPE_VIDEO]->startTimeForPlaylistSync.tv_sec
			+ (float)(trackState[eMEDIATYPE_AUDIO]->startTimeForPlaylistSync.tv_usec - trackState[eMEDIATYPE_VIDEO]->startTimeForPlaylistSync.tv_usec) / 1000000;
		if (!useProgramDateTimeIfAvailable)
		{
			if (video->targetDurationSeconds != audio->targetDurationSeconds)
			{
				logprintf("%s:%d WARNING seqno based track synchronization when video->targetDurationSeconds[%f] != audio->targetDurationSeconds[%f]",
				        __FUNCTION__, __LINE__, video->targetDurationSeconds, audio->targetDurationSeconds);
			}
			else
			{
				double diffBasedOnSeqNumber = (mediaSequenceNumber[eMEDIATYPE_AUDIO]
				        - mediaSequenceNumber[eMEDIATYPE_VIDEO]) * video->fragmentDurationSeconds;
				if (fabs(diffBasedOnSeqNumber - diffBetweenStartTimes) > video->fragmentDurationSeconds)
				{
					logprintf("%s:%d WARNING - inconsistency between startTime and seqno  startTime diff %f diffBasedOnSeqNumber %f",
					        __FUNCTION__, __LINE__, diffBetweenStartTimes, diffBasedOnSeqNumber);
				}
			}
		}

		if((diffBetweenStartTimes < -10 || diffBetweenStartTimes > 10))
		{
			logprintf("syncTracks diff debug : Audio start time sec : %ld  Video start time sec : %ld ",
			trackState[eMEDIATYPE_AUDIO]->startTimeForPlaylistSync.tv_sec,
			trackState[eMEDIATYPE_VIDEO]->startTimeForPlaylistSync.tv_sec);
		}
	}

	//Sync using sequence number since startTime is not available or not desired
	if (!startTimeAvailable || !useProgramDateTimeIfAvailable)
	{
		MediaType mediaType;
#ifdef TRACE
		logprintf("%s:%d sync using sequence number. A %lld V %lld a-f-uri %s v-f-uri %s", __FUNCTION__,
				__LINE__, mediaSequenceNumber[eMEDIATYPE_AUDIO], mediaSequenceNumber[eMEDIATYPE_VIDEO],
				trackState[eMEDIATYPE_AUDIO]->fragmentURI, trackState[eMEDIATYPE_VIDEO]->fragmentURI);
#endif
		TrackState *laggingTS = NULL;
		long long diff = 0;
		if (mediaSequenceNumber[eMEDIATYPE_AUDIO] > mediaSequenceNumber[eMEDIATYPE_VIDEO])
		{
			laggingTS = video;
			diff = mediaSequenceNumber[eMEDIATYPE_AUDIO] - mediaSequenceNumber[eMEDIATYPE_VIDEO];
			mediaType = eMEDIATYPE_VIDEO;
			logprintf("%s:%d video track lag in seqno. diff %lld", __FUNCTION__, __LINE__, diff);
		}
		else if (mediaSequenceNumber[eMEDIATYPE_VIDEO] > mediaSequenceNumber[eMEDIATYPE_AUDIO])
		{
			laggingTS = audio;
			diff = mediaSequenceNumber[eMEDIATYPE_VIDEO] - mediaSequenceNumber[eMEDIATYPE_AUDIO];
			mediaType = eMEDIATYPE_AUDIO;
			logprintf("%s:%d audio track lag in seqno. diff %lld", __FUNCTION__, __LINE__, diff);
		}
		if (laggingTS)
		{
			if (startTimeAvailable && (diff > MAX_SEQ_NUMBER_DIFF_FOR_SEQ_NUM_BASED_SYNC))
			{
				logprintf("%s:%d - falling back to synchronization based on start time as diff = %lld", __FUNCTION__, __LINE__, diff);
			}
			else if ((diff <= MAX_SEQ_NUMBER_LAG_COUNT) && (diff > 0))
			{
				logprintf("%s:%d sync using sequence number. diff [%lld] A [%lld] V [%lld] a-f-uri [%s] v-f-uri [%s]", __FUNCTION__,
						__LINE__, diff, mediaSequenceNumber[eMEDIATYPE_AUDIO], mediaSequenceNumber[eMEDIATYPE_VIDEO],
						trackState[eMEDIATYPE_AUDIO]->fragmentURI, trackState[eMEDIATYPE_VIDEO]->fragmentURI);
				while (diff > 0)
				{
					laggingTS->playTarget += laggingTS->fragmentDurationSeconds;
					laggingTS->playTargetOffset += laggingTS->fragmentDurationSeconds;
					if (laggingTS->fragmentURI)
					{
						laggingTS->fragmentURI = laggingTS->GetNextFragmentUriFromPlaylist(true);
					}
					else
					{
						logprintf("%s:%d laggingTS->fragmentURI NULL, seek might be out of window", __FUNCTION__, __LINE__);
					}
					diff--;
				}
				syncedUsingSeqNum = true;
			}
			else
			{
				logprintf("%s:%d Lag in '%s' seq no, diff[%lld] > maxValue[%d]",
						__FUNCTION__, __LINE__, ((eMEDIATYPE_VIDEO == mediaType) ? "video" : "audio"), diff, MAX_SEQ_NUMBER_LAG_COUNT);
			}
		}
		else
		{
			logprintf("%s:%d No lag in seq no b/w AV", __FUNCTION__, __LINE__);
			syncedUsingSeqNum = true;
		}

		if (subtitle->enabled && syncedUsingSeqNum)
		{
			long long subtitleDiff = mediaSequenceNumber[eMEDIATYPE_AUDIO] - mediaSequenceNumber[eMEDIATYPE_SUBTITLE];
			//We can only support subtitle to catch-up to audio. The opposite will cause a/v sync issues
			if (subtitleDiff > 0 && subtitleDiff <= MAX_SEQ_NUMBER_LAG_COUNT)
			{
				logprintf("%s:%d sync subtitle using sequence number. diff [%lld] A [%lld] S [%lld] a-f-uri [%s] s-f-uri [%s]", __FUNCTION__,
						__LINE__, subtitleDiff, mediaSequenceNumber[eMEDIATYPE_AUDIO], mediaSequenceNumber[eMEDIATYPE_SUBTITLE],
						audio->fragmentURI, subtitle->fragmentURI);
				//Subtitle catch up to audio
				while (subtitleDiff > 0)
				{
					subtitle->playTarget += subtitle->fragmentDurationSeconds;
					subtitle->playTargetOffset += subtitle->fragmentDurationSeconds;
					if (subtitle->fragmentURI)
					{
						subtitle->fragmentURI = subtitle->GetNextFragmentUriFromPlaylist();
					}
					else
					{
						logprintf("%s:%d subtitle->fragmentURI NULL, seek might be out of window", __FUNCTION__, __LINE__);
					}
					subtitleDiff--;
				}
			}
			else if (subtitleDiff < 0)
			{
				//Audio can't catch up with subtitle, since its already sync-ed with video.
				logprintf("%s:%d sync using sequence number failed, subtitle will be starting late. diff [%lld] A [%lld] S [%lld] a-f-uri [%s] s-f-uri [%s]", __FUNCTION__,
									__LINE__, subtitleDiff, mediaSequenceNumber[eMEDIATYPE_AUDIO], mediaSequenceNumber[eMEDIATYPE_SUBTITLE],
									audio->fragmentURI, subtitle->fragmentURI);
			}
			else
			{
				logprintf("%s:%d No lag in seq no b/w audio and subtitle", __FUNCTION__, __LINE__);
			}
		}
	}

	if (!syncedUsingSeqNum)
	{
		if (startTimeAvailable)
		{
			if (diffBetweenStartTimes > 0)
			{
				TrackState *ts = trackState[eMEDIATYPE_VIDEO];
				if (diffBetweenStartTimes > (ts->fragmentDurationSeconds / 2))
				{
					if (video->mDuration > (ts->playTarget + diffBetweenStartTimes))
					{
						logprintf("%s:%d Audio track in front, catchup videotrack", __FUNCTION__, __LINE__);
						ts->playTarget += diffBetweenStartTimes;
						ts->playTargetOffset = diffBetweenStartTimes;
					}
					else
					{
						logprintf("%s:%d invalid diff %f ts->playTarget %f trackDuration %f", __FUNCTION__, __LINE__,
						        diffBetweenStartTimes, ts->playTarget, video->mDuration);
						retval = eAAMPSTATUS_TRACKS_SYNCHRONISATION_ERROR;
					}
				}
				else
				{
					logprintf("syncTracks : Skip playTarget updation diff %f, vid track start %f fragmentDurationSeconds %f",
					        diffBetweenStartTimes, ts->playTarget, ts->fragmentDurationSeconds);
				}
			}
			else if (diffBetweenStartTimes < 0)
			{
				TrackState *ts = trackState[eMEDIATYPE_AUDIO];
				if (fabs(diffBetweenStartTimes) > (ts->fragmentDurationSeconds / 2))
				{
					if (audio->mDuration > (ts->playTarget - diffBetweenStartTimes))
					{
						logprintf("%s:%d Video track in front, catchup audio track", __FUNCTION__, __LINE__);
						ts->playTarget -= diffBetweenStartTimes;
						ts->playTargetOffset = -diffBetweenStartTimes;
					}
					else
					{
						logprintf("%s:%d invalid diff %f ts->playTarget %f trackDuration %f", __FUNCTION__, __LINE__,
						        diffBetweenStartTimes, ts->playTarget, audio->mDuration);
						retval = eAAMPSTATUS_TRACKS_SYNCHRONISATION_ERROR;
					}
				}
				else
				{
					logprintf("syncTracks : Skip playTarget updation diff %f, aud track start %f fragmentDurationSeconds %f",
					        fabs(diffBetweenStartTimes), ts->playTarget, ts->fragmentDurationSeconds);
				}
			}

			if (subtitle->enabled)
			{
				//Compare subtitle and audio start time
				const double subtitleDiff = audio->startTimeForPlaylistSync.tv_sec - subtitle->startTimeForPlaylistSync.tv_sec
							+ (float)(audio->startTimeForPlaylistSync.tv_usec - subtitle->startTimeForPlaylistSync.tv_usec) / 1000000;
				if (subtitleDiff > 0)
				{
					//Audio is at a higher start time that subtitle. Subtitle track needs to catch-up
					if (subtitleDiff > (subtitle->fragmentDurationSeconds / 2))
					{
						if (subtitle->mDuration > (subtitle->playTarget + subtitleDiff))
						{
							logprintf("%s:%d Audio track in front, catch up subtitle track", __FUNCTION__, __LINE__);
							subtitle->playTarget += subtitleDiff;
							subtitle->playTargetOffset = subtitleDiff;
						}
						else
						{
							logprintf("%s:%d invalid diff(%f) greater than duration, ts->playTarget %f trackDuration %f, subtitle may start early", __FUNCTION__, __LINE__, subtitleDiff, subtitle->playTarget, subtitle->mDuration);
						}
					}
					else
					{
						logprintf("syncTracks : Skip subtitle playTarget updation diff %f, subtitle track start %f fragmentDurationSeconds %f", subtitleDiff, subtitle->playTarget, subtitle->fragmentDurationSeconds);
					}
				}
				else if (subtitleDiff < 0)
				{
					//Can't catch-up audio to subtitle, since audio and video are already sync-ed
					logprintf("syncTracks : Skip subtitle sync to audio for subtitle startTime %f, audio startTime %f. Subtitle will be starting late",
							(subtitle->startTimeForPlaylistSync.tv_sec + (subtitle->startTimeForPlaylistSync.tv_usec / 1000000)),
							(audio->startTimeForPlaylistSync.tv_sec + (audio->startTimeForPlaylistSync.tv_usec / 1000000)));
				}
			}
		}
		else
		{
			logprintf("%s:%d Could not sync using seq num and start time not available., cannot play this content.!!",
			        __FUNCTION__, __LINE__);
			retval = eAAMPSTATUS_TRACKS_SYNCHRONISATION_ERROR;
		}
	}
	if (subtitle->enabled)
	{
		logprintf("syncTracks Exit : audio track start %f, vid track start %f sub track start %f", audio->playTarget, video->playTarget, subtitle->playTarget);
	}
	else
	{
		logprintf("syncTracks Exit : audio track start %f, vid track start %f", audio->playTarget, video->playTarget );
	}

	return retval;
}
/***************************************************************************
* @fn Init
* @brief Function to initialize member variables,download main manifest and parse 
*		 
* @param tuneType[in] Tune type 
* @return bool true on success 
***************************************************************************/
AAMPStatusType StreamAbstractionAAMP_HLS::Init(TuneType tuneType)
{
	AAMPStatusType retval = eAAMPSTATUS_GENERIC_ERROR;
	bool needMetadata = true;
	mTuneType = tuneType;
	bool newTune = aamp->IsNewTune();
    /* START: Added As Part of DELIA-28363 and DELIA-28247 */
	aamp->IsTuneTypeNew = newTune;
    /* END: Added As Part of DELIA-28363 and DELIA-28247 */

	TSProcessor* audioQueuedPC = NULL;
	long http_error;

	memset(&mainManifest, 0, sizeof(mainManifest));
	if (newTune)
	{
		AveDrmManager::ResetAll();
	}

	for (int i = 0; i < AAMP_TRACK_COUNT; i++)
	{
		aamp->SetCurlTimeout(aamp->mNetworkTimeoutMs, i);
	}

	if (aamp->getAampCacheHandler()->RetrieveFromPlaylistCache(aamp->GetManifestUrl(), &mainManifest, aamp->GetManifestUrl()))
	{
		logprintf("StreamAbstractionAAMP_HLS::%s:%d Main manifest retrieved from cache", __FUNCTION__, __LINE__);
	}
	if (!this->mainManifest.len)
	{
		aamp->profiler.ProfileBegin(PROFILE_BUCKET_MANIFEST);
		traceprintf("StreamAbstractionAAMP_HLS::%s:%d downloading manifest", __FUNCTION__, __LINE__);
		// take the original url before its gets changed in GetFile
		std::string mainManifestOrigUrl = aamp->GetManifestUrl();
		aamp->SetCurlTimeout(aamp->mManifestTimeoutMs, 0);
		aamp->GetFile(aamp->GetManifestUrl(), &this->mainManifest, aamp->GetManifestUrl(), &http_error, NULL, 0, true, eMEDIATYPE_MANIFEST);
		aamp->SetCurlTimeout(aamp->mNetworkTimeoutMs, 0);
		//update videoend info
		aamp->UpdateVideoEndMetrics( eMEDIATYPE_MANIFEST,0,http_error,aamp->GetManifestUrl());
		if (this->mainManifest.len)
		{
			aamp->profiler.ProfileEnd(PROFILE_BUCKET_MANIFEST);
			traceprintf("StreamAbstractionAAMP_HLS::%s:%d downloaded manifest", __FUNCTION__, __LINE__);
			aamp->getAampCacheHandler()->InsertToPlaylistCache(mainManifestOrigUrl, &mainManifest, aamp->GetManifestUrl(),false,eMEDIATYPE_MANIFEST);
		}
		else
		{
			logprintf("Manifest download failed : http response : %d", (int) http_error);
		}
	}
	if (!this->mainManifest.len && aamp->DownloadsAreEnabled()) //!aamp->GetFile(aamp->GetManifestUrl(), &this->mainManifest, aamp->GetManifestUrl()))
	{
		aamp->profiler.ProfileError(PROFILE_BUCKET_MANIFEST, http_error);
		aamp->SendDownloadErrorEvent(AAMP_TUNE_MANIFEST_REQ_FAILED, http_error);
	}
	if (this->mainManifest.len)
	{
		aamp_AppendNulTerminator(&this->mainManifest); // make safe for cstring operations
		if (gpGlobalConfig->logging.trace )
		{
			printf("***Main Manifest***:\n\n%s\n************\n", this->mainManifest.ptr);
		}

#ifdef AAMP_HARVEST_SUPPORT_ENABLED
		HarvestFile(aamp->GetManifestUrl(), &mainManifest, false, "main-");
#endif
		// Parse the Main manifest ( As Parse function modifies the original data,InsertCache had to be called before it . 
		AAMPStatusType mainManifestResult = ParseMainManifest(this->mainManifest.ptr);
		// Check if Main manifest is good or not 
		if(mainManifestResult != eAAMPSTATUS_OK)
		{				
			if(mainManifestResult == eAAMPSTATUS_MANIFEST_INVALID_TYPE)
			{
				// if playlist is provided as Main manifest, return error in this version.
				// Playlist based playback support to be added in future
				// TODO :: Handle playlist only playback -> Disable ABR , Re-arrange cached , populate track.  
				logprintf("%s:%d WARNING !!!. Playlist received instead of Manifest.Playback failed",__FUNCTION__,__LINE__);
				mainManifestResult = eAAMPSTATUS_MANIFEST_CONTENT_ERROR;
			}
			// check for the error type , if critical error return immediately
			if(mainManifestResult == eAAMPSTATUS_MANIFEST_CONTENT_ERROR || mainManifestResult == eAAMPSTATUS_MANIFEST_PARSE_ERROR)
			{	
				// Dump the invalid manifest content before reporting error 
				int tempDataLen = (MANIFEST_TEMP_DATA_LENGTH - 1);
				char temp[MANIFEST_TEMP_DATA_LENGTH];
				strncpy(temp, this->mainManifest.ptr, tempDataLen);
				temp[tempDataLen] = 0x00;
				// this will print only one line :(
				printf("ERROR: Invalid Main Manifest : %s \n", temp);	   
				return mainManifestResult; 
			}
		}

		if (!newTune)
		{
			long persistedBandwidth = aamp->GetPersistedBandwidth();
			//We were tuning to a lesser profile previously, so we use it as starting profile
			if (persistedBandwidth > 0 && persistedBandwidth < gpGlobalConfig->defaultBitrate)
			{
				mAbrManager.setDefaultInitBitrate(persistedBandwidth);
			}
		}
		currentProfileIndex = GetDesiredProfile(false);
		lastSelectedProfileIndex = currentProfileIndex;
		aamp->ResetCurrentlyAvailableBandwidth(this->streamInfo[this->currentProfileIndex].bandwidthBitsPerSecond,trickplayMode,this->currentProfileIndex);
		aamp->profiler.SetBandwidthBitsPerSecondVideo(this->streamInfo[this->currentProfileIndex].bandwidthBitsPerSecond);
		aamp->NotifyBitRateChangeEvent(this->streamInfo[this->currentProfileIndex].bandwidthBitsPerSecond,
						"BitrateChanged - Network Adaptation",
						this->streamInfo[this->currentProfileIndex].resolution.width,
						this->streamInfo[this->currentProfileIndex].resolution.height,
						this->streamInfo[this->currentProfileIndex].resolution.framerate,
						true);

		/* START: Added As Part of DELIA-28363 and DELIA-28247 */
		logprintf("Selected BitRate: %ld, Max BitRate: %ld", streamInfo[currentProfileIndex].bandwidthBitsPerSecond, GetStreamInfo(GetMaxBWProfile())->bandwidthBitsPerSecond);
		/* END: Added As Part of DELIA-28363 and DELIA-28247 */
		
		for (int iTrack = AAMP_TRACK_COUNT - 1; iTrack >= 0; iTrack--)
		{
			const char* trackName = "subs";
			if (eTRACK_VIDEO == iTrack)
			{
				if(trackState[eTRACK_AUDIO]->enabled)
				{
					trackName = "video";
				}
				else if (rate != AAMP_NORMAL_PLAY_RATE)
				{
					trackName = "iframe";
				}
				else
				{
					trackName = "muxed";
				}
			}
			else if (eTRACK_AUDIO == iTrack)
			{
				trackName = "audio";
			}
			trackState[iTrack] = new TrackState((TrackType)iTrack, this, aamp, trackName);
			TrackState *ts = trackState[iTrack];
			ts->playlistPosition = -1;
			ts->playTarget = seekPosition;
			if (iTrack == eTRACK_SUBTITLE && !aamp->IsSubtitleEnabled())
			{
				AAMPLOG_INFO("StreamAbstractionAAMP_HLS::%s:%d subtitles disabled by application", __FUNCTION__, __LINE__);
				ts->enabled = false;
				ts->streamOutputFormat = FORMAT_NONE;
				continue;
			}
			const char *uri = GetPlaylistURI((TrackType)iTrack, &ts->streamOutputFormat);
			if (uri)
			{
				aamp_ResolveURL(ts->mPlaylistUrl, aamp->GetManifestUrl(), uri);
				if(ts->streamOutputFormat != FORMAT_NONE)
				{
					ts->enabled = true;
					mNumberOfTracks++;
				}
				else
				{
					logprintf("StreamAbstractionAAMP_HLS::%s:%d %s format could not be determined. codecs %s", __FUNCTION__, __LINE__, ts->name, streamInfo[currentProfileIndex].codecs);
				}
			}
		}

		TrackState *audio = trackState[eMEDIATYPE_AUDIO];
		TrackState *video = trackState[eMEDIATYPE_VIDEO];
		TrackState *subtitle = trackState[eMEDIATYPE_SUBTITLE];

		if(gpGlobalConfig->bAudioOnlyPlayback)
		{
			if(audio->enabled)
			{
				video->enabled = false;
				video->streamOutputFormat = FORMAT_NONE;
			}
			else
			{
				trackState[eTRACK_VIDEO]->type = eTRACK_AUDIO;
			}
			subtitle->enabled = false;
			subtitle->streamOutputFormat = FORMAT_NONE;
		}
		aamp->profiler.SetBandwidthBitsPerSecondAudio(audio->GetCurrentBandWidth());

		pthread_t trackPLDownloadThreadID;
		bool trackPLDownloadThreadStarted = false;
		if (audio->enabled)
		{
			if (aamp->getAampCacheHandler()->RetrieveFromPlaylistCache(audio->mPlaylistUrl, &audio->playlist, audio->mEffectiveUrl))
			{
				logprintf("StreamAbstractionAAMP_HLS::%s:%d audio playlist retrieved from cache", __FUNCTION__, __LINE__);
			}
			if(!audio->playlist.len)
			{
				if (gpGlobalConfig->playlistsParallelFetch)
				{
					int ret = pthread_create(&trackPLDownloadThreadID, NULL, TrackPLDownloader, audio);
					if(ret != 0)
					{
						logprintf("StreamAbstractionAAMP_HLS::%s:%d pthread_create failed for TrackPLDownloader with errno = %d, %s", __FUNCTION__, __LINE__, errno, strerror(errno));
					}
					else
					{
						trackPLDownloadThreadStarted = true;
					}
				}
				else
				{
					audio->FetchPlaylist();
				}
			}
		}
		if (video->enabled)
		{
			if (aamp->getAampCacheHandler()->RetrieveFromPlaylistCache(video->mPlaylistUrl, &video->playlist, video->mEffectiveUrl))
			{
				logprintf("StreamAbstractionAAMP_HLS::%s:%d video playlist retrieved from cache", __FUNCTION__, __LINE__);
			}
			if(!video->playlist.len)
			{
				video->FetchPlaylist();
			}
		}
		if (subtitle->enabled)
		{
			if (aamp->getAampCacheHandler()->RetrieveFromPlaylistCache(subtitle->mPlaylistUrl, &subtitle->playlist, subtitle->mEffectiveUrl))
			{
				logprintf("StreamAbstractionAAMP_HLS::%s:%d subtitle playlist retrieved from cache", __FUNCTION__, __LINE__);
			}
			if (!subtitle->playlist.len)
			{
				subtitle->FetchPlaylist();
			}
			if (!subtitle->playlist.len)
			{
				//This is logged as a warning. Not critical to playback
				AAMPLOG_ERR("StreamAbstractionAAMP_HLS::%s:%d Subtitle playlist download failed", __FUNCTION__, __LINE__);
				subtitle->enabled = false;
			}
		}
		if (trackPLDownloadThreadStarted)
		{
			pthread_join(trackPLDownloadThreadID, NULL);
		}
		if ((video->enabled && !video->playlist.len) || (audio->enabled && !audio->playlist.len))
		{
			logprintf("StreamAbstractionAAMP_HLS::%s:%d Playlist download failed",__FUNCTION__,__LINE__);
			return eAAMPSTATUS_MANIFEST_DOWNLOAD_ERROR;
		}

		bool bSetStatePreparing = false;

		if (rate != AAMP_NORMAL_PLAY_RATE)
		{
			trickplayMode = true;
			if(aamp->IsTSBSupported())
			{
				mTrickPlayFPS = gpGlobalConfig->linearTrickplayFPS;
			}
			else
			{
				mTrickPlayFPS = gpGlobalConfig->vodTrickplayFPS;
			}
		}
		else
		{
			trickplayMode = false;
		}

		for (int iTrack = AAMP_TRACK_COUNT - 1; iTrack >= 0; iTrack--)
		{
			TrackState *ts = trackState[iTrack];

			if(ts->enabled)
			{
				bool playContextConfigured = false;
				aamp_AppendNulTerminator(&ts->playlist); // make safe for cstring operations
				if (gpGlobalConfig->logging.trace  )
				{
					printf("***Initial Playlist:******\n\n%s\n*****************\n", ts->playlist.ptr);
				}
#ifdef AAMP_HARVEST_SUPPORT_ENABLED
				const char* prefix = (iTrack == eTRAK_SUBTITLE)?"sub-":((iTrack == eTRACK_AUDIO)?"aud-":(trickplayMode)?"ifr-":"vid-");
				HarvestFile(ts->mPlaylistUrl, &ts->playlist, false, prefix);
#endif
				// Flag also denotes if first encrypted init fragment was pushed or not
				ts->mCheckForInitialFragEnc = (newTune || mTuneType == eTUNETYPE_RETUNE); //these tune types have new gstreamer pipeline
				ts->IndexPlaylist();
				ts->FindTimedMetadata();
				if (ts->mDuration == 0.0f)
				{
					if (iTrack == eTRACK_SUBTITLE)
					{
						//Subtitle is optional and not critical to playback
						ts->enabled = false;
						AAMPLOG_ERR("StreamAbstractionAAMP_HLS::%s:%d Subtitle playlist duration is zero!!", __FUNCTION__, __LINE__);
					}
					else
					{
						break;
					}
				}

				if (newTune && needMetadata)
				{
					needMetadata = false;
					std::set<std::string> langList;
					std::vector<long> bitrateList;
					//To avoid duplicate entries in audioLanguage list
					for (int iMedia = 0; iMedia < mMediaCount; iMedia++)
					{
						if (this->mediaInfo[iMedia].type == eMEDIATYPE_AUDIO && this->mediaInfo[iMedia].language)
						{
							langList.insert(this->mediaInfo[iMedia].language);
						}
					}

					bitrateList.reserve(GetProfileCount());
					for (int i = 0; i < GetProfileCount(); i++)
					{
						if (!streamInfo[i].isIframeTrack)
						{
							bitrateList.push_back(streamInfo[i].bandwidthBitsPerSecond);
						}
						else
						{
							aamp->mIsIframeTrackPresent = true;
						}
					}
					aamp->SendMediaMetadataEvent((ts->mDuration * 1000.0), langList, bitrateList, hasDrm, aamp->mIsIframeTrackPresent);

					// Delay "preparing" state until all tracks have been processed.
					// JS Player assumes all onTimedMetadata event fire before "preparing" state.
					bSetStatePreparing = true;
				}

				if (iTrack == eMEDIATYPE_VIDEO)
				{
					maxIntervalBtwPlaylistUpdateMs = 2 * ts->targetDurationSeconds * 1000; //Time interval for periodic playlist update
					if (maxIntervalBtwPlaylistUpdateMs > DEFAULT_INTERVAL_BETWEEN_PLAYLIST_UPDATES_MS)
					{
						maxIntervalBtwPlaylistUpdateMs = DEFAULT_INTERVAL_BETWEEN_PLAYLIST_UPDATES_MS;
					}
					aamp->UpdateRefreshPlaylistInterval(maxIntervalBtwPlaylistUpdateMs/1000.0);
				}

				ts->fragmentURI = ts->playlist.ptr;
				StreamOutputFormat format = GetFormatFromFragmentExtension(ts);
				if (FORMAT_ISO_BMFF == format)
				{
					//Disable subtitle in mp4 format, as we don't support it for now
					if (eMEDIATYPE_SUBTITLE == iTrack)
					{
						AAMPLOG_WARN("StreamAbstractionAAMP_HLS::%s %d Unsupported subtitle format from fragment extension:%d", __FUNCTION__, __LINE__, format);
						ts->streamOutputFormat = FORMAT_NONE;
						ts->fragmentURI = NULL;
						ts->enabled = false;
					}
					else
					{
						logprintf("StreamAbstractionAAMP_HLS::Init : Track[%s] - FORMAT_ISO_BMFF", ts->name);
						ts->streamOutputFormat = FORMAT_ISO_BMFF;
						//Disable subtitle for fragmented MP4 assets, as we need tsprocessor support for webvtt parsing now
						if (subtitle->enabled)
						{
							AAMPLOG_WARN("StreamAbstractionAAMP_HLS::%s %d Unsupported media format for audio or video - FORMAT_ISO_BMFF", __FUNCTION__, __LINE__);
							subtitle->streamOutputFormat = FORMAT_NONE;
							subtitle->fragmentURI = NULL;
							//mSubtitleParser will be deleted in destructor, so unhandled here
							subtitle->enabled = false;
						}
					}
					continue;
				}
				// Not ISOBMFF, no need for encrypted header check and associated logic
				// But header identification might have been already done, if EXT-X-MAP is present in playlist
				ts->mCheckForInitialFragEnc = false;
				if (FORMAT_AUDIO_ES_AAC == format)
				{
					logprintf("StreamAbstractionAAMP_HLS::Init : Track[%s] - FORMAT_AUDIO_ES_AAC", ts->name);
					ts->streamOutputFormat = FORMAT_AUDIO_ES_AAC;
					continue;
				}

				if (eMEDIATYPE_SUBTITLE == iTrack)
				{
					bool subtitleDisabled = false;
					if (this->rate != AAMP_NORMAL_PLAY_RATE)
					{
						logprintf("Disable subtitle format - trick play");
						subtitleDisabled = true;
					}
					else if (format != FORMAT_SUBTITLE_WEBVTT)
					{
						AAMPLOG_WARN("StreamAbstractionAAMP_HLS::%s %d Unsupported subtitle format from fragment extension:%d", __FUNCTION__, __LINE__, format);
						subtitleDisabled = true;
					}

					//Configure parser for subtitle
					if (!subtitleDisabled)
					{
						ts->streamOutputFormat = format;
						SubtitleMimeType type = (format == FORMAT_SUBTITLE_WEBVTT) ? eSUB_TYPE_WEBVTT : eSUB_TYPE_UNKNOWN;
						ts->mSubtitleParser = new WebVTTParser(aamp, type);
					}
					else
					{
						ts->streamOutputFormat = FORMAT_NONE;
						ts->fragmentURI = NULL;
						ts->enabled = false;
					}
					continue; //no playcontext config for subtitle
				}
				else if (eMEDIATYPE_AUDIO == iTrack)
				{
					if (this->rate == AAMP_NORMAL_PLAY_RATE)
					{
						if (format == FORMAT_MPEGTS)
						{
							if (gpGlobalConfig->gAampDemuxHLSAudioTsTrack)
							{
								logprintf("Configure audio TS track demuxing");
								ts->playContext = new TSProcessor(aamp,eStreamOp_DEMUX_AUDIO);
							}
							else if (gpGlobalConfig->gAampMergeAudioTrack)
							{
								logprintf("Configure audio TS track to queue");
								ts->playContext = new TSProcessor(aamp,eStreamOp_QUEUE_AUDIO);
								ts->streamOutputFormat = FORMAT_NONE;
								audioQueuedPC = ts->playContext;
							}
							if (ts->playContext)
							{
								ts->playContext->setRate(this->rate, PlayMode_normal);
								ts->playContext->setThrottleEnable(false);
								playContextConfigured = true;
							}
							else
							{
								ts->streamOutputFormat = format;
							}
						}
						else if (FORMAT_INVALID != format)
						{
							logprintf("Configure audio format based on extension");
							ts->streamOutputFormat = format;
						}
						else
						{
							logprintf("Keeping audio format from playlist");
						}
					}
					else
					{
						logprintf("Disable audio format - trick play");
						ts->streamOutputFormat = FORMAT_NONE;
						ts->fragmentURI = NULL;
						ts->enabled = false;
					}
				}
				else if ((gpGlobalConfig->gAampDemuxHLSVideoTsTrack && (rate == AAMP_NORMAL_PLAY_RATE))
						|| (gpGlobalConfig->demuxHLSVideoTsTrackTM && (rate != AAMP_NORMAL_PLAY_RATE)))
				{
					HlsStreamInfo* streamInfo = &this->streamInfo[this->currentProfileIndex];
					/*Populate format from codec data*/
					format = FORMAT_INVALID;
					if (streamInfo->codecs)
					{
						for (int j = 0; j < AAMP_VIDEO_FORMAT_MAP_LEN; j++)
						{
							if (strstr(streamInfo->codecs, videoFormatMap[j].codec))
							{
								format = videoFormatMap[j].format;
								AAMPLOG_INFO("StreamAbstractionAAMP_HLS::Init : VideoTrack: format is %d [%s]",
									videoFormatMap[j].format, videoFormatMap[j].codec);
								break;
							}
						}
					}
					if (FORMAT_INVALID != format)
					{
						StreamOperation demuxOp;
						ts->streamOutputFormat = format;
						if ((trackState[eTRACK_AUDIO]->enabled) || (AAMP_NORMAL_PLAY_RATE != rate))
						{
							demuxOp = eStreamOp_DEMUX_VIDEO;
						}
						else
						{
							if (streamInfo->codecs)
							{
								for (int j = 0; j < AAMP_AUDIO_FORMAT_MAP_LEN; j++)
								{
									if (strstr(streamInfo->codecs, audioFormatMap[j].codec))
									{
										trackState[eMEDIATYPE_AUDIO]->streamOutputFormat = audioFormatMap[j].format;
										logprintf("StreamAbstractionAAMP_HLS::Init : Audio format is %d [%s]",
											audioFormatMap[j].format, audioFormatMap[j].codec);
										break;
									}
								}
							}
							if(FORMAT_NONE != trackState[eMEDIATYPE_AUDIO]->streamOutputFormat)
							{
								if(!gpGlobalConfig->bAudioOnlyPlayback)
								{
									demuxOp = eStreamOp_DEMUX_ALL;
								}
								else
								{
									demuxOp = eStreamOp_DEMUX_AUDIO;
									video->streamOutputFormat = FORMAT_NONE;
								}
							}
							else
							{
								logprintf("StreamAbstractionAAMP_HLS::%s:%d Demux only video. codecs %s", __FUNCTION__, __LINE__, streamInfo[currentProfileIndex].codecs);
								demuxOp = eStreamOp_DEMUX_VIDEO;
							}
						}
                        AAMPLOG_WARN("StreamAbstractionAAMP_HLS::Init : Configure video TS track demuxing demuxOp %d", demuxOp);
						ts->playContext = new TSProcessor(aamp,demuxOp, eMEDIATYPE_VIDEO, trackState[eMEDIATYPE_AUDIO]->playContext);
						ts->playContext->setThrottleEnable(this->enableThrottle);
						if (this->rate == AAMP_NORMAL_PLAY_RATE)
						{
							ts->playContext->setRate(this->rate, PlayMode_normal);
						}
						else
						{
							ts->playContext->setRate(this->rate, PlayMode_retimestamp_Ionly);
							ts->playContext->setFrameRateForTM(mTrickPlayFPS);
						}
						playContextConfigured = true;
					}
					else
					{
						logprintf("StreamAbstractionAAMP_HLS::Init : VideoTrack -couldn't determine format from streamInfo->codec %s",
							streamInfo->codecs);
					}
				}
				else /*Video Track - No demuxing*/
				{
					if (audioQueuedPC)
					{
						logprintf("StreamAbstractionAAMP_HLS::Init : Configure video TS track eStreamOp_SEND_VIDEO_AND_QUEUED_AUDIO");
						ts->playContext = new TSProcessor(aamp,eStreamOp_SEND_VIDEO_AND_QUEUED_AUDIO, eMEDIATYPE_VIDEO, audioQueuedPC);
						ts->playContext->setThrottleEnable(this->enableThrottle);
						ts->playContext->setRate(this->rate, PlayMode_normal);
						playContextConfigured = true;
					}
					else
					{
						logprintf("StreamAbstractionAAMP_HLS::Init : Configure video TS track %p : No streamops", ts);
					}
				}
				if (!playContextConfigured && (ts->streamOutputFormat == FORMAT_MPEGTS))
				{
					logprintf("StreamAbstractionAAMP_HLS::Init : track %p context configuring for eStreamOp_NONE", ts);
					ts->playContext = new TSProcessor(aamp, eStreamOp_NONE, iTrack);
					ts->playContext->setThrottleEnable(this->enableThrottle);
					if (this->rate == AAMP_NORMAL_PLAY_RATE)
					{
						this->trickplayMode = false;
						ts->playContext->setRate(this->rate, PlayMode_normal);
					}
					else
					{
						this->trickplayMode = true;
						if(aamp->IsTSBSupported())
						{
							mTrickPlayFPS = gpGlobalConfig->linearTrickplayFPS;
						}
						else
						{
							mTrickPlayFPS = gpGlobalConfig->vodTrickplayFPS;
						}
						ts->playContext->setRate(this->rate, PlayMode_retimestamp_Ionly);
						ts->playContext->setFrameRateForTM(mTrickPlayFPS);
					}
				}
			}
		}

		if ((video->enabled && video->mDuration == 0.0f) || (audio->enabled && audio->mDuration == 0.0f))
		{
			logprintf("StreamAbstractionAAMP_HLS::%s:%d Track Duration is 0. Cannot play this content", __FUNCTION__, __LINE__);
			return eAAMPSTATUS_MANIFEST_CONTENT_ERROR;
		}

		if (bSetStatePreparing)
			aamp->SetState(eSTATE_PREPARING);

		//Currently un-used playlist indexed event, might save some JS overhead
		if (!gpGlobalConfig->disablePlaylistIndexEvent)
		{
			aamp->SendEventAsync(AAMP_EVENT_PLAYLIST_INDEXED);
		}
		if (newTune)
		{
			TunedEventConfig tunedEventConfig =  aamp->IsLive() ?
					gpGlobalConfig->tunedEventConfigLive : gpGlobalConfig->tunedEventConfigVOD;
			if (eTUNED_EVENT_ON_PLAYLIST_INDEXED == tunedEventConfig)
			{
				if (aamp->SendTunedEvent())
				{
					logprintf("aamp: hls - sent tune event after indexing playlist");
				}
			}
		}

		/*Do live adjust on live streams on 1. eTUNETYPE_NEW_NORMAL, 2. eTUNETYPE_SEEKTOLIVE,
		 * 3. Seek to a point beyond duration*/
		bool liveAdjust = (eTUNETYPE_NEW_NORMAL == tuneType)  && aamp->IsLiveAdjustRequired() && (aamp->IsLive());
		if ((eTUNETYPE_SEEKTOLIVE == tuneType) && aamp->IsLive())
		{
			logprintf("StreamAbstractionAAMP_HLS::%s:%d eTUNETYPE_SEEKTOLIVE, reset playTarget and enable liveAdjust",__FUNCTION__,__LINE__);
			liveAdjust = true;

			audio->playTarget = 0;
			video->playTarget = 0;
			subtitle->playTarget = 0;
			aamp->NotifyOnEnteringLive();
		}
		else if (((eTUNETYPE_SEEK == tuneType) || (eTUNETYPE_RETUNE == tuneType) || (eTUNETYPE_NEW_SEEK == tuneType)) && (this->rate > 0))
		{
			double seekWindowEnd = video->mDuration;
			if(aamp->IsLive())
			{
				seekWindowEnd -= aamp->mLiveOffset ; 
			}
			// check if seek beyond live point

			if (video->playTarget > seekWindowEnd)
			{
				if (aamp->IsLive())
				{
					logprintf("StreamAbstractionAAMP_HLS::%s:%d playTarget > seekWindowEnd , playTarget:%f and seekWindowEnd:%f",
							__FUNCTION__,__LINE__, video->playTarget , seekWindowEnd);
					liveAdjust = true;

					audio->playTarget = 0;
					video->playTarget = 0;
					subtitle->playTarget = 0;
					if (eTUNETYPE_SEEK == tuneType)
					{
						aamp->NotifyOnEnteringLive();
					}
				}
				else
				{
					video->eosReached = true;
					video->fragmentURI = NULL;
					audio->eosReached = true;
					audio->fragmentURI = NULL;
					subtitle->eosReached = true;
					subtitle->fragmentURI = NULL;
					logprintf("StreamAbstractionAAMP_HLS::%s:%d seek target out of range, mark EOS. playTarget:%f End:%f. ",
							__FUNCTION__,__LINE__,video->playTarget, seekWindowEnd);

					return eAAMPSTATUS_SEEK_RANGE_ERROR;
				}
			}
		}

		if (audio->enabled)
		{
			if (!aamp->IsLive())
			{
				SyncTracksForDiscontinuity();
			}
			else
			{
				if(!gpGlobalConfig->bAudioOnlyPlayback)
				{
					bool syncDone = false;
					if (!liveAdjust && video->mDiscontinuityIndexCount && (video->mDiscontinuityIndexCount == audio->mDiscontinuityIndexCount))
					{
						if (eAAMPSTATUS_OK == SyncTracksForDiscontinuity())
						{
							syncDone = true;
						}
					}
					if (!syncDone)
					{
						bool useProgramDateTimeIfAvailable = gpGlobalConfig->hlsAVTrackSyncUsingStartTime;
						/*For VSS streams, use program-date-time if available*/
						if (aamp->mIsVSS)
						{
							logprintf("StreamAbstractionAAMP_HLS::%s:%d : VSS stream", __FUNCTION__, __LINE__);
							useProgramDateTimeIfAvailable = true;
						}
						AAMPStatusType retValue = SyncTracks(useProgramDateTimeIfAvailable);
						if (eAAMPSTATUS_OK != retValue)
						{
							return retValue;
						}
					}
				}
			}
		}
		else if (subtitle->enabled)
		{
			//TODO:Muxed track with subtitles. Need to sync tracks
		}


		if (liveAdjust)
		{
			double xStartOffset = video->GetXStartTimeOffset();
			double offsetFromLive = aamp->mLiveOffset ; 
			// check if there is xStartOffSet , if non zero value present ,check if it is > 3 times TD(Spec requirement)
			if(xStartOffset != 0 && abs(xStartOffset) > (3*video->targetDurationSeconds))
			{
				// DELIA-40177 -> For now code added for negative offset values
				// that is offset from last duration 
				if(xStartOffset < 0)
				{
					offsetFromLive = abs(xStartOffset);
					AAMPLOG_WARN("%s: liveOffset modified with X-Start to :%f",__FUNCTION__,offsetFromLive);
				}
				// if xStartOffset is positive value , then playposition to be considered from beginning 
				// TBD for later.Only offset from end is supported now . That too only for live . Not for VOD!!!!
			}
			
			if (video->mDuration > (offsetFromLive + video->playTargetOffset))
			{
				//DELIA-28451
				// a) Get OffSet to Live for Video and Audio separately. 
				// b) Set to minimum value among video /audio instead of setting to 0 position
				int offsetToLiveVideo,offsetToLiveAudio,offsetToLive;
				offsetToLiveVideo = offsetToLiveAudio = video->mDuration - offsetFromLive - video->playTargetOffset;
				if (audio->enabled)
				{ 
					offsetToLiveAudio = 0;
					// if audio is not having enough total duration to adjust , then offset value set to 0
					if( audio->mDuration > (offsetFromLive + audio->playTargetOffset))
						offsetToLiveAudio = audio->mDuration - offsetFromLive -  audio->playTargetOffset;
					else
						logprintf("aamp: live adjust not possible ATotal[%f]< (AoffsetFromLive[%d] + AplayTargetOffset[%f]) A-target[%f]", audio->mDuration,offsetFromLive,audio->playTargetOffset,audio->playTarget);
				}
				// pick the min of video/audio offset
				offsetToLive = (std::min)(offsetToLiveVideo,offsetToLiveAudio);
				video->playTarget += offsetToLive;
				if (audio->enabled )
				{
					audio->playTarget += offsetToLive;
				}
				if (subtitle->enabled)
				{
					subtitle->playTarget += offsetToLive;
				}
				// Entering live will happen if offset is adjusted , if its 0 playback is starting from beginning 
				if(offsetToLive)
					mIsAtLivePoint = true;
				logprintf("aamp: after live adjust - V-target %f A-target %f S-target %f offsetFromLive %d offsetToLive %d offsetVideo[%d] offsetAudio[%d] AtLivePoint[%d]",
				        video->playTarget, audio->playTarget, subtitle->playTarget, offsetFromLive, offsetToLive,offsetToLiveVideo,offsetToLiveAudio,mIsAtLivePoint);
			}
			else
			{
				logprintf("aamp: live adjust not possible VTotal[%f] < (VoffsetFromLive[%d] + VplayTargetOffset[%f]) V-target[%f]", 
					video->mDuration,offsetFromLive,video->playTargetOffset,video->playTarget);
			}
			//Set live adusted position to seekPosition
			seekPosition = video->playTarget;
		}
		/*Adjust for discontinuity*/
		if ((audio->enabled) && (aamp->IsLive()) && !gpGlobalConfig->bAudioOnlyPlayback)
		{
			int discontinuityIndexCount = video->mDiscontinuityIndexCount;
			if (discontinuityIndexCount > 0)
			{
				if (discontinuityIndexCount == audio->mDiscontinuityIndexCount)
				{
					if (liveAdjust)
					{
						SyncTracksForDiscontinuity();
					}
					float videoPrevDiscontinuity = 0;
					float audioPrevDiscontinuity = 0;
					float videoNextDiscontinuity;
					float audioNextDiscontinuity;
					DiscontinuityIndexNode* videoDiscontinuityIndex = (DiscontinuityIndexNode*)video->mDiscontinuityIndex.ptr;
					DiscontinuityIndexNode* audioDiscontinuityIndex = (DiscontinuityIndexNode*)audio->mDiscontinuityIndex.ptr;
					for (int i = 0; i <= discontinuityIndexCount; i++)
					{
						if (i < discontinuityIndexCount)
						{
							videoNextDiscontinuity = videoDiscontinuityIndex[i].position;
							audioNextDiscontinuity = audioDiscontinuityIndex[i].position;
						}
						else
						{
							videoNextDiscontinuity = aamp->GetDurationMs() / 1000;
							audioNextDiscontinuity = videoNextDiscontinuity;
						}
						if ((videoNextDiscontinuity > (video->playTarget + 5))
						        && (audioNextDiscontinuity > (audio->playTarget + 5)))
						{

							logprintf( "StreamAbstractionAAMP_HLS::%s:%d : video->playTarget %f videoPrevDiscontinuity %f videoNextDiscontinuity %f",
									__FUNCTION__, __LINE__, video->playTarget, videoPrevDiscontinuity, videoNextDiscontinuity);
							logprintf( "StreamAbstractionAAMP_HLS::%s:%d : audio->playTarget %f audioPrevDiscontinuity %f audioNextDiscontinuity %f",
									__FUNCTION__, __LINE__, audio->playTarget, audioPrevDiscontinuity, audioNextDiscontinuity);
							if (video->playTarget < videoPrevDiscontinuity)
							{
								logprintf( "StreamAbstractionAAMP_HLS::%s:%d : [video] playTarget(%f) advance to discontinuity(%f)",
										__FUNCTION__, __LINE__, video->playTarget, videoPrevDiscontinuity);
								video->playTarget = videoPrevDiscontinuity;
							}
							if (audio->playTarget < audioPrevDiscontinuity)
							{
								logprintf( "StreamAbstractionAAMP_HLS::%s:%d : [audio] playTarget(%f) advance to discontinuity(%f)",
										__FUNCTION__, __LINE__, audio->playTarget, audioPrevDiscontinuity);
								audio->playTarget = audioPrevDiscontinuity;
							}
							break;
						}
						videoPrevDiscontinuity = videoNextDiscontinuity;
						audioPrevDiscontinuity = audioNextDiscontinuity;
					}
				}
				else
				{
					logprintf("StreamAbstractionAAMP_HLS::%s:%d : videoPeriodPositionIndex.size %d audioPeriodPositionIndex.size %d",
							__FUNCTION__, __LINE__, video->mDiscontinuityIndexCount, audio->mDiscontinuityIndexCount);
				}
			}
			else
			{
				logprintf("StreamAbstractionAAMP_HLS::%s:%d : videoPeriodPositionIndex.size 0", __FUNCTION__, __LINE__);
			}
		}

		audio->lastPlaylistDownloadTimeMS = aamp_GetCurrentTimeMS();
		video->lastPlaylistDownloadTimeMS = audio->lastPlaylistDownloadTimeMS;
		subtitle->lastPlaylistDownloadTimeMS = audio->lastPlaylistDownloadTimeMS;
		/*Use start timestamp as zero when audio is not elementary stream*/
		mStartTimestampZero = ((video->streamOutputFormat == FORMAT_ISO_BMFF || audio->streamOutputFormat == FORMAT_ISO_BMFF) || (rate == AAMP_NORMAL_PLAY_RATE && (!audio->enabled || audio->playContext)));
		if (subtitle->enabled && subtitle->mSubtitleParser != NULL)
		{
			//Need to set reportProgressOffset to subtitleParser
			//playTarget becomes seek_pos_seconds and playlistPosition is the acutal position in playlist
			//TODO: move call GetNextFragmentUriFromPlaylist() to the sync operation btw muxed and subtitle
			if (!audio->enabled)
			{
				video->fragmentURI = video->GetNextFragmentUriFromPlaylist(true);
				video->playTarget = video->playlistPosition;
			}
			double offset = (video->playlistPosition - seekPosition) * 1000.0;
			logprintf("StreamAbstractionAAMP_HLS::%s:%d: Setting setProgressEventOffset value of %.3f ms", __FUNCTION__, __LINE__, offset);
			subtitle->mSubtitleParser->setProgressEventOffset(offset);
		}
		if (newTune && gpGlobalConfig->prefetchIframePlaylist)
		{
			int iframeStreamIdx = GetIframeTrack();
			if (0 <= iframeStreamIdx)
			{
				std::string defaultIframePlaylistUrl;
				std::string defaultIframePlaylistEffectiveUrl;
				GrowableBuffer defaultIframePlaylist;
				aamp_ResolveURL(defaultIframePlaylistUrl, aamp->GetManifestUrl(), streamInfo[iframeStreamIdx].uri);
				traceprintf("StreamAbstractionAAMP_HLS::%s:%d : Downloading iframe playlist", __FUNCTION__, __LINE__);
				bool bFiledownloaded = false;
				if (aamp->getAampCacheHandler()->RetrieveFromPlaylistCache(defaultIframePlaylistUrl, &defaultIframePlaylist, defaultIframePlaylistEffectiveUrl) == false){
					bFiledownloaded = aamp->GetFile(defaultIframePlaylistUrl, &defaultIframePlaylist, defaultIframePlaylistEffectiveUrl, &http_error);
					//update videoend info
					aamp->UpdateVideoEndMetrics( eMEDIATYPE_MANIFEST,streamInfo[iframeStreamIdx].bandwidthBitsPerSecond,http_error,defaultIframePlaylistEffectiveUrl);
				}
				if (defaultIframePlaylist.len && bFiledownloaded)
				{
					aamp->getAampCacheHandler()->InsertToPlaylistCache(defaultIframePlaylistUrl, &defaultIframePlaylist, defaultIframePlaylistEffectiveUrl,aamp->IsLive(),eMEDIATYPE_IFRAME);
					traceprintf("StreamAbstractionAAMP_HLS::%s:%d : Cached iframe playlist", __FUNCTION__, __LINE__);
				}
				else
				{
					logprintf("StreamAbstractionAAMP_HLS::%s:%d : Error Download iframe playlist. http_error %ld",
					        __FUNCTION__, __LINE__, http_error);
				}
			}
		}
		retval = eAAMPSTATUS_OK;
	}
	return retval;
}
/***************************************************************************
* @fn GetFirstPTS
* @brief Function to return first PTS 
*		 
* @return double PTS value 
***************************************************************************/
double StreamAbstractionAAMP_HLS::GetFirstPTS()
{
	double pts;
	if( mStartTimestampZero)
	{
		pts = 0;
	}
	else
	{
		pts = seekPosition;
	}
	return pts;
}

/***************************************************************************
* @fn RunFetchLoop
* @brief Fragment collector thread execution function to download fragments 
*		 
* @return void
***************************************************************************/
void TrackState::RunFetchLoop()
{
	bool skipFetchFragment = false;
	for (;;)
	{
		while (fragmentURI && aamp->DownloadsAreEnabled())
		{
			skipFetchFragment = false;

			if (mInjectInitFragment)
			{
				// DELIA-40273: mInjectInitFragment marks if init fragment has to be pushed whereas mInitFragmentInfo
				// holds the init fragment URL. Both has to be present for init fragment fetch & injection to work.
				// During ABR, mInjectInitFragment is set and for live assets,  mInitFragmentInfo is found
				// in FindMediaForSequenceNumber() and for VOD its found in GetNextFragmentUriFromPlaylist()
				// which also sets mInjectInitFragment to true, so below reset will not have an impact
				if (mInitFragmentInfo)
				{
					FetchInitFragment();
					//Inject init fragment failed due to no free cache
					if (mInjectInitFragment)
					{
						skipFetchFragment = true;
					}
					else
					{
						skipFetchFragment = false;
					}
				}
				else
				{
					mInjectInitFragment = false;
				}
			}

			if (!skipFetchFragment)
			{
				FetchFragment();
			}

			// FetchFragment involves multiple wait operations, so check download status again
			if (!aamp->DownloadsAreEnabled())
			{
				break;
			}

			/*Check for profile change only for video track*/
			// Avoid ABR if we have seen or just pushed an init fragment
			if((eTRACK_VIDEO == type) && (!context->trickplayMode) && !(mInjectInitFragment || mSkipAbr))
			{
				context->lastSelectedProfileIndex = context->currentProfileIndex;
				//DELIA-33346 -- if rampdown is attempted to any failure , no abr change to be attempted . 
				// else profile be resetted to top one leading to looping of bad fragment 
				if(!context->mCheckForRampdown)
				{
					if (aamp->CheckABREnabled())
					{
						context->CheckForProfileChange();
					}
					else if (!context->aamp->IsTSBSupported())
					{
						context->CheckUserProfileChangeReq();
					}
				}
			}

			if (IsLive())
			{
				int timeSinceLastPlaylistDownload = (int) (aamp_GetCurrentTimeMS()
				        - lastPlaylistDownloadTimeMS);
				if (context->maxIntervalBtwPlaylistUpdateMs <= timeSinceLastPlaylistDownload)
				{
					AAMPLOG_INFO("%s:%d: Refreshing playlist as maximum refresh delay exceeded", __FUNCTION__, __LINE__);
					RefreshPlaylist();
				}
#ifdef TRACE
				else
				{
					logprintf("%s:%d: Not refreshing timeSinceLastPlaylistDownload = %d", __FUNCTION__, __LINE__, timeSinceLastPlaylistDownload);
				}
#endif
			}
			pthread_mutex_lock(&mutex);
			if(refreshPlaylist)
			{
				RefreshPlaylist();
				refreshPlaylist = false;
			}
			pthread_mutex_unlock(&mutex);
		}
		// reached end of vod stream
		//teststreamer_EndOfStreamReached();

		if (eosReached || mReachedEndListTag || !context->aamp->DownloadsAreEnabled())
		{
			AbortWaitForCachedAndFreeFragment(false);
			break;
		}
		if (lastPlaylistDownloadTimeMS)
		{
			// if not present, new playlist wih at least one additional segment will be available
			// no earlier than 0.5*EXT-TARGETDURATION and no later than 1.5*EXT-TARGETDURATION
			// relative to previous playlist fetch.
			int timeSinceLastPlaylistDownload = (int)(aamp_GetCurrentTimeMS() - lastPlaylistDownloadTimeMS);
			int minDelayBetweenPlaylistUpdates = MAX_DELAY_BETWEEN_PLAYLIST_UPDATE_MS;
			long long currentPlayPosition = aamp->GetPositionMs();
			long long endPositionAvailable = (aamp->culledSeconds + aamp->durationSeconds)*1000;
			// playTarget value will vary if TSB is full and trickplay is attempted. Cant use for buffer calculation 
			// So using the endposition in playlist - Current playing position to get the buffer availability
			long bufferAvailable = (endPositionAvailable - currentPlayPosition);
			// If buffer Available is > 2*targetDuration
			if(bufferAvailable  > (targetDurationSeconds*2*1000) )
			{
				// may be 1.0 times also can be set ??? 
				minDelayBetweenPlaylistUpdates = (int)(1.5 * 1000 * targetDurationSeconds);
			}
			// if buffer is between 2*target & targetDuration
			else if(bufferAvailable  > (targetDurationSeconds*1000))
			{
				minDelayBetweenPlaylistUpdates = (int)(0.5 * 1000 * targetDurationSeconds);
			}
			// This is to handle the case where target duration is high value(>Max delay)  but buffer is available just above the max update inteval
			else if(bufferAvailable > (2*MAX_DELAY_BETWEEN_PLAYLIST_UPDATE_MS))
			{
				minDelayBetweenPlaylistUpdates = MAX_DELAY_BETWEEN_PLAYLIST_UPDATE_MS;
			}
			// if buffer < targetDuration && buffer < MaxDelayInterval
			else
			{
				// if bufferAvailable is less than targetDuration ,its in RED alert . Close to freeze 
				// need to refresh soon ..
				if(bufferAvailable)
				{
					minDelayBetweenPlaylistUpdates = (int)(bufferAvailable / 3) ; 
				}
				else
				{
					minDelayBetweenPlaylistUpdates = MIN_DELAY_BETWEEN_PLAYLIST_UPDATE_MS; // 500mSec
				}
				// limit the logs when buffer is low 
				{
					static int bufferlowCnt;
					if((bufferlowCnt++ & 5) == 0)
					{ 
						logprintf("%s:%d: Buffer is running low(%ld).Type(%d) Refreshing playlist(%d).Target(%f) PlayPosition(%lld) End(%lld)",
							__FUNCTION__, __LINE__, bufferAvailable,type,minDelayBetweenPlaylistUpdates,playTarget,currentPlayPosition,endPositionAvailable);
					}
				}
			}
			// adjust with last refreshed time interval
			minDelayBetweenPlaylistUpdates -= timeSinceLastPlaylistDownload;
			// restrict to Max delay interval 	
			if (minDelayBetweenPlaylistUpdates > MAX_DELAY_BETWEEN_PLAYLIST_UPDATE_MS)
			{
				minDelayBetweenPlaylistUpdates = MAX_DELAY_BETWEEN_PLAYLIST_UPDATE_MS;
			}
			else if(minDelayBetweenPlaylistUpdates < MIN_DELAY_BETWEEN_PLAYLIST_UPDATE_MS) 
			{
				// minimum of 500 mSec needed to avoid too frequent download.
				minDelayBetweenPlaylistUpdates = MIN_DELAY_BETWEEN_PLAYLIST_UPDATE_MS;
			}
			AAMPLOG_INFO("%s:%d aamp playlist end refresh type(%d) bufferMs(%ld) playtarget(%f) delay(%d) End(%lld) PlayPosition(%lld)",
					__FUNCTION__, __LINE__, type,bufferAvailable,playTarget,minDelayBetweenPlaylistUpdates,endPositionAvailable,currentPlayPosition);
			aamp->InterruptableMsSleep(minDelayBetweenPlaylistUpdates);
		}
		RefreshPlaylist();

		AAMPLOG_FAILOVER("%s:%d: fragmentURI [%s] timeElapsedSinceLastFragment [%f]",
			__FUNCTION__, __LINE__, fragmentURI, (aamp_GetCurrentTimeMS() - context->LastVideoFragParsedTimeMS()));

		/* Added to handle an edge case for cdn failover, where we found valid sub-manifest but no valid fragments.
		 * In this case we have to stall the playback here. */
		if( fragmentURI == NULL && IsLive() && type == eTRACK_VIDEO)
		{
			AAMPLOG_FAILOVER("%s:%d: fragmentURI is NULL, playback may stall in few seconds..", __FUNCTION__, __LINE__);
			context->CheckForPlaybackStall(false);
		}
	}
	AAMPLOG_WARN("%s:%d: fragment collector done. track %s", __FUNCTION__, __LINE__, name);
}
/***************************************************************************
* @fn FragmentCollector
* @brief Fragment collector thread function
*		 
* @param arg[in] TrackState pointer
* @return void
***************************************************************************/

static void *FragmentCollector(void *arg)
{
	TrackState *track = (TrackState *)arg;
	if(aamp_pthread_setname(pthread_self(), "aampHLSFetch"))
	{
		logprintf("%s:%d: aamp_pthread_setname failed", __FUNCTION__, __LINE__);
	}
	track->RunFetchLoop();
	return NULL;
}
/***************************************************************************
* @fn StreamAbstractionAAMP_HLS
* @brief Constructor function 
*		 
* @param aamp[in] PrivateInstanceAAMP pointer
* @param seekpos[in] Seek position 
* @param rate[in] Rate of playback 
* @param enableThrottle[in] throttle enable/disable flag
* @return void
***************************************************************************/
StreamAbstractionAAMP_HLS::StreamAbstractionAAMP_HLS(class PrivateInstanceAAMP *aamp,double seekpos, float rate, bool enableThrottle) : StreamAbstractionAAMP(aamp),
	rate(rate), maxIntervalBtwPlaylistUpdateMs(DEFAULT_INTERVAL_BETWEEN_PLAYLIST_UPDATES_MS), mainManifest(), allowsCache(false), seekPosition(seekpos), mTrickPlayFPS(),
	enableThrottle(enableThrottle), firstFragmentDecrypted(false), mStartTimestampZero(false), mNumberOfTracks(0),
	lastSelectedProfileIndex(0), segDLFailCount(0), segDrmDecryptFailCount(0), mMediaCount(0)
{
#ifndef AVE_DRM
       logprintf("PlayerInstanceAAMP() : AVE DRM disabled");
#endif
	trickplayMode = false;

	logprintf("hls fragment collector seekpos = %f", seekpos);
	if (rate == AAMP_NORMAL_PLAY_RATE)
	{
		this->trickplayMode = false;
	}
	else
	{
		this->trickplayMode = true;
	}
	//targetDurationSeconds = 0.0;
	mAbrManager.clearProfiles();
	memset(&trackState[0], 0x00, sizeof(trackState));
	aamp->CurlInit(0, AAMP_TRACK_COUNT);
	memset(streamInfo, 0, sizeof(*streamInfo));
}
/***************************************************************************
* @fn TrackState
* @brief TrackState Constructor
*		 
* @param type[in] Type of the track
* @param parent[in] StreamAbstractionAAMP_HLS instance 
* @param aamp[in] PrivateInstanceAAMP pointer 
* @param name[in] Name of the track 
* @return void
***************************************************************************/
TrackState::TrackState(TrackType type, StreamAbstractionAAMP_HLS* parent, PrivateInstanceAAMP* aamp, const char* name) :
		MediaTrack(type, aamp, name),
		indexCount(0), currentIdx(0), indexFirstMediaSequenceNumber(0), fragmentURI(NULL), lastPlaylistDownloadTimeMS(0),
		byteRangeLength(0), byteRangeOffset(0), nextMediaSequenceNumber(0), playlistPosition(0), playTarget(0),
		streamOutputFormat(FORMAT_NONE), playContext(NULL),
		playTargetOffset(0),
		discontinuity(false),
		refreshPlaylist(false), fragmentCollectorThreadID(0),
		fragmentCollectorThreadStarted(false),
		manifestDLFailCount(0),
		mCMSha1Hash(NULL), mDrmTimeStamp(0), mDrmMetaDataIndexCount(0),firstIndexDone(false), mDrm(NULL), mDrmLicenseRequestPending(false),
		mInjectInitFragment(false), mInitFragmentInfo(NULL), mDrmKeyTagCount(0), mIndexingInProgress(false), mForceProcessDrmMetadata(false),
		mDuration(0), mLastMatchedDiscontPosition(-1), mCulledSeconds(0),
		mEffectiveUrl(""), mPlaylistUrl(""), mFragmentURIFromIndex(""),
		mDiscontinuityIndexCount(0), mSyncAfterDiscontinuityInProgress(false), playlist(),
		index(), targetDurationSeconds(1), mDeferredDrmKeyMaxTime(0), startTimeForPlaylistSync(),
		context(parent), fragmentEncrypted(false), mKeyTagChanged(false), mLastKeyTagIdx(0), mDrmInfo(),
		mDrmMetaDataIndexPosition(0), mDrmMetaDataIndex(), mDiscontinuityIndex(), mKeyHashTable(), mPlaylistMutex(),
		mPlaylistIndexed(), mTrackDrmMutex(), mPlaylistType(ePLAYLISTTYPE_UNDEFINED), mReachedEndListTag(false),
		mByteOffsetCalculation(false),mSkipAbr(false),
		mCheckForInitialFragEnc(false), mFirstEncInitFragmentInfo(NULL), mDrmMethod(eDRM_KEY_METHOD_NONE)
		,mXStartTimeOFfset(0)
{
	memset(&playlist, 0, sizeof(playlist));
	memset(&index, 0, sizeof(index));
	memset(&startTimeForPlaylistSync, 0, sizeof(struct timeval));
	memset(&mDrmMetaDataIndex, 0, sizeof(mDrmMetaDataIndex));
	memset(&mDrmInfo, 0, sizeof(mDrmInfo));
	memset(&mDiscontinuityIndex, 0, sizeof(mDiscontinuityIndex));
	pthread_cond_init(&mPlaylistIndexed, NULL);
	pthread_mutex_init(&mPlaylistMutex, NULL);
	pthread_mutex_init(&mTrackDrmMutex, NULL);
}
/***************************************************************************
* @fn ~TrackState
* @brief Destructor function 
*		 
* @return void
***************************************************************************/
TrackState::~TrackState()
{
	aamp_Free(&playlist.ptr);
	for (int j=0; j< gpGlobalConfig->maxCachedFragmentsPerTrack; j++)
	{
		aamp_Free(&cachedFragment[j].fragment.ptr);
	}
	FlushIndex();
	if (playContext)
	{
		delete playContext;
	}
	if (mSubtitleParser)
	{
		delete mSubtitleParser;
	}
	if (mCMSha1Hash)
	{
		free(mCMSha1Hash);
		mCMSha1Hash = NULL;
	}
	if (mDrmInfo.iv)
	{
		free(mDrmInfo.iv);
		mDrmInfo.iv = NULL;
	}
	if (mDrmInfo.uri)
	{
		free(mDrmInfo.uri);
		mDrmInfo.uri = NULL;
	}
	pthread_cond_destroy(&mPlaylistIndexed);
	pthread_mutex_destroy(&mPlaylistMutex);
	pthread_mutex_destroy(&mTrackDrmMutex);
}
/***************************************************************************
* @fn Stop
* @brief Function to stop track download/playback 
*		 
* @return void
***************************************************************************/
void TrackState::Stop()
{
	AbortWaitForCachedAndFreeFragment(true);

	if (playContext)
	{
		playContext->abort();
	}
	if (fragmentCollectorThreadStarted)
	{
		void *value_ptr = NULL;
		int rc = pthread_join(fragmentCollectorThreadID, &value_ptr);
		if (rc != 0)
		{
			logprintf("***pthread_join fragmentCollectorThread returned %d(%s)", rc, strerror(rc));
		}
#ifdef TRACE
		else
		{
			logprintf("joined fragmentCollectorThread");
		}
#endif
		fragmentCollectorThreadStarted = false;
	}
	StopInjectLoop();

	//To be called after StopInjectLoop to avoid cues to be injected after cleanup
	if (mSubtitleParser)
	{
		mSubtitleParser->reset();
		mSubtitleParser->close();
	}
}
/***************************************************************************
* @fn ~StreamAbstractionAAMP_HLS
* @brief Destructor function for StreamAbstractionAAMP_HLS
*		 
* @return void
***************************************************************************/
StreamAbstractionAAMP_HLS::~StreamAbstractionAAMP_HLS()
{
	/*Exit from ongoing  http fetch, drm operation,throttle. Mark fragment collector exit*/

	for (int i = 0; i < AAMP_TRACK_COUNT; i++)
	{
		TrackState *track = trackState[i];
		if (track)
		{
			delete track;
		}
	}

	aamp->SyncBegin();
	aamp_Free(&this->mainManifest.ptr);
	aamp->CurlTerm(0, AAMP_TRACK_COUNT);
	aamp->SyncEnd();
}
/***************************************************************************
* @fn Start
* @brief Function to create threads for track donwload
*		 
* @return void
***************************************************************************/
void TrackState::Start(void)
{
	if(playContext)
	{
		playContext->reset();
	}
	assert(!fragmentCollectorThreadStarted);
	if (0 == pthread_create(&fragmentCollectorThreadID, NULL, &FragmentCollector, this))
	{
		fragmentCollectorThreadStarted = true;
	}
	else
	{
		logprintf("Failed to create FragmentCollector thread");
	}
	if(aamp->IsPlayEnabled())
	{
		StartInjectLoop();
	}
}
/***************************************************************************
* @fn Start
* @brief Function to start track initiaziation 
*		 
* @return void
***************************************************************************/
void StreamAbstractionAAMP_HLS::Start(void)
{
	for (int iTrack = 0; iTrack < AAMP_TRACK_COUNT; iTrack++)
	{
		TrackState *track = trackState[iTrack];
		if(track->Enabled())
		{
			track->Start();
		}
	}
}
/***************************************************************************
* @fn Stop
* @brief Function to stop the HLS streaming 
*		 
* @param clearChannelData[in] flag indicating to full stop or temporary stop	
* @return void
***************************************************************************/
void StreamAbstractionAAMP_HLS::Stop(bool clearChannelData)
{
	aamp->DisableDownloads();
	ReassessAndResumeAudioTrack(true);
	AbortWaitForAudioTrackCatchup();

	//This is purposefully kept in a separate loop to avoid being hung
	//on pthread_join of fragmentCollectorThread
	for (int iTrack = 0; iTrack < AAMP_TRACK_COUNT; iTrack++)
	{
		TrackState *track = trackState[iTrack];
		if(track && track->Enabled())
		{
			track->CancelDrmOperation(clearChannelData);
		}
	}

	for (int iTrack = 0; iTrack < AAMP_TRACK_COUNT; iTrack++)
	{
		TrackState *track = trackState[iTrack];

		/*Stop any waits for other track's playlist update*/
		TrackState *otherTrack = trackState[(iTrack == eTRACK_VIDEO)? eTRACK_AUDIO: eTRACK_VIDEO];
		if(otherTrack && otherTrack->Enabled())
		{
			otherTrack->StopWaitForPlaylistRefresh();
		}

		if(track && track->Enabled())
		{
			track->Stop();
			if (!clearChannelData)
			{
				//Restore drm key state which was reset by drm_CancelKeyWait earlier since drm data is persisted
				track->RestoreDrmState();
			}
		}
	}

	if (clearChannelData && aamp->GetCurrentDRM() == eDRM_Adobe_Access)
	{
		AveDrmManager::CancelKeyWaitAll();
		AveDrmManager::ReleaseAll();
		AveDrmManager::ResetAll();
	}

	aamp->EnableDownloads();
}
/***************************************************************************
* @fn DumpProfiles
* @brief Function to log debug information on Stream/Media information
*		 
* @return void
***************************************************************************/
void StreamAbstractionAAMP_HLS::DumpProfiles(void)
{
	int profileCount = GetProfileCount();
	if (profileCount)
	{
		for (int i = 0; i < profileCount; i++)
		{
			struct HlsStreamInfo *streamInfo = &this->streamInfo[i];
			logprintf("stream[%d]:", i);
			if (streamInfo->uri) logprintf("\tURI:%s", streamInfo->uri);
			logprintf("\tBANDWIDTH:%ld", streamInfo->bandwidthBitsPerSecond);
			logprintf("\tPROGRAM-ID:%ld", streamInfo->program_id);
			if (streamInfo->audio) logprintf("\tAUDIO:%s", streamInfo->audio);
			if (streamInfo->codecs) logprintf("\tCODECS:%s", streamInfo->codecs);
			logprintf("\tRESOLUTION: %dx%d FPS:%f", streamInfo->resolution.width, streamInfo->resolution.height,streamInfo->resolution.framerate);
		}
		logprintf("");
	}

	if (mMediaCount)
	{
		for (int i = 0; i < mMediaCount; i++)
		{
			MediaInfo *mediaInfo = &this->mediaInfo[i];
			logprintf("media[%d]:", i);
			if (mediaInfo->uri) logprintf("\tURI:%s", mediaInfo->uri);
			switch (mediaInfo->type)
			{
			case eMEDIATYPE_AUDIO:
				logprintf("type:AUDIO");
				break;
			case eMEDIATYPE_VIDEO:
				logprintf("type:VIDEO");
				break;
			default:
				break;
			}
			if (mediaInfo->group_id) logprintf("\tgroup-id:%s", mediaInfo->group_id);
			if (mediaInfo->name) logprintf("\tname:%s", mediaInfo->name);
			if (mediaInfo->language) logprintf("\tlanguage:%s", mediaInfo->language);
			if (mediaInfo->autoselect)
			{
				logprintf("\tAUTOSELECT");
			}
			if (mediaInfo->isDefault)
			{
				logprintf("\tDEFAULT");
			}
		}
		logprintf("");
	}
}

/***************************************************************************
* @fn GetStreamFormat
* @brief Function to get stream format 
*		 
* @param primaryOutputFormat[out] video format 
* @param audioOutputFormat[out] audio format
* @return void
***************************************************************************/
void StreamAbstractionAAMP_HLS::GetStreamFormat(StreamOutputFormat &primaryOutputFormat, StreamOutputFormat &audioOutputFormat)
{
	primaryOutputFormat = trackState[eMEDIATYPE_VIDEO]->streamOutputFormat;
	audioOutputFormat = trackState[eMEDIATYPE_AUDIO]->streamOutputFormat;
}
/***************************************************************************
* @fn GetVideoBitrates
* @brief Function to get available video bitrates
*
* @return available video bitrates
***************************************************************************/
std::vector<long> StreamAbstractionAAMP_HLS::GetVideoBitrates(void)
{
	std::vector<long> bitrates;
	int profileCount = GetProfileCount();
	if (profileCount)
	{
		for (int i = 0; i < profileCount; i++)
		{
			struct HlsStreamInfo *streamInfo = &this->streamInfo[i];
			//Not send iframe bw info, since AAMP has ABR disabled for trickmode
			if (!streamInfo->isIframeTrack)
			{
				bitrates.push_back(streamInfo->bandwidthBitsPerSecond);
			}
		}
	}
	return bitrates;
}
/***************************************************************************
* @fn GetAudioBitrates
* @brief Function to get available audio bitrates
*
* @return available audio bitrates
***************************************************************************/
std::vector<long> StreamAbstractionAAMP_HLS::GetAudioBitrates(void)
{
	//TODO: Impl audio bitrate getter
	return std::vector<long>();
}

/***************************************************************************
* @fn NotifyFirstVideoPTS
* @brief Function to notify first video pts value from tsprocessor
*
* @param pts[in] base pts
* @return none
***************************************************************************/
void StreamAbstractionAAMP_HLS::NotifyFirstVideoPTS(unsigned long long pts)
{
	//start subtitles
	TrackState *subtitle = trackState[eMEDIATYPE_SUBTITLE];

	if (subtitle->enabled && subtitle->mSubtitleParser != NULL)
	{
		//position within playlist and pts in ms
		subtitle->mSubtitleParser->init(seekPosition * 1000.0, pts);
	}
}

/***************************************************************************
* @fn DrmDecrypt
* @brief Function to decrypt the fragment for playback
*		 
* @param cachedFragment[in] CachedFragment struction pointer 	
* @param bucketTypeFragmentDecrypt[in] ProfilerBucketType enum
* @return bool true if successfully decrypted 
***************************************************************************/
DrmReturn TrackState::DrmDecrypt( CachedFragment * cachedFragment, ProfilerBucketType bucketTypeFragmentDecrypt)
{
		DrmReturn drmReturn = eDRM_ERROR;

		pthread_mutex_lock(&mTrackDrmMutex);
		if (aamp->DownloadsAreEnabled())
		{
			// Update the DRM Context , if current active Drm Session is not received (mDrm)
			// or if Key Tag changed ( either with hash change )
			// For DAI scenaio-> Clear to Encrypted or Encrypted to Clear can happen.
			//      For Encr to clear,not to set SetDrmContext
			//      For Clear to Encr,SetDrmContext is called.If same hash then not SetDecrypto called
			if (fragmentEncrypted && (!mDrm || mKeyTagChanged))
			{
				SetDrmContext();
				mKeyTagChanged = false;
			}
			if(mDrm)
			{
				drmReturn = mDrm->Decrypt(bucketTypeFragmentDecrypt, cachedFragment->fragment.ptr,
						cachedFragment->fragment.len, MAX_LICENSE_ACQ_WAIT_TIME);

			}
		}
		pthread_mutex_unlock(&mTrackDrmMutex);

		if (drmReturn != eDRM_SUCCESS)
		{
			aamp->profiler.ProfileError(bucketTypeFragmentDecrypt, drmReturn);
		}
		return drmReturn;
}

/***************************************************************************
* @fn GetContext
* @brief Function to get current StreamAbstractionAAMP instance value 
*		 
* @return StreamAbstractionAAMP instance
***************************************************************************/
StreamAbstractionAAMP* TrackState::GetContext()
{
	return context;
}
/***************************************************************************
* @fn GetMediaTrack
* @brief Function to get Media information for track type
*		 
* @param type[in] TrackType input	
* @return MediaTrack structure pointer
***************************************************************************/
MediaTrack* StreamAbstractionAAMP_HLS::GetMediaTrack(TrackType type)
{
	return trackState[(int)type];
}
/***************************************************************************
* @fn UpdateDrmCMSha1Hash
* @brief Function to Update SHA1 Id for DRM Metadata 
*		 
* @param ptr[in] ShaID string from DRM attribute
* @return void
***************************************************************************/
void TrackState::UpdateDrmCMSha1Hash(const char *ptr)
{
	bool drmDataChanged = false;
	if (NULL == ptr)
	{
		if (mCMSha1Hash)
		{
			free(mCMSha1Hash);
			mCMSha1Hash = NULL;
		}
	}
	else if (mCMSha1Hash)
	{
		if (0 != memcmp(ptr, (char*) mCMSha1Hash, DRM_SHA1_HASH_LEN))
		{
			if (!mIndexingInProgress)
			{
				printf("%s:%d [%s] Different DRM metadata hash. old - ", __FUNCTION__, __LINE__, name);
				for (int i = 0; i< DRM_SHA1_HASH_LEN; i++)
				{
					printf("%c", mCMSha1Hash[i]);
				}
				printf(" new - ");
				for (int i = 0; i< DRM_SHA1_HASH_LEN; i++)
				{
					printf("%c", ptr[i]);
				}
				printf("\n");
			}
			drmDataChanged = true;
			memcpy(mCMSha1Hash, ptr, DRM_SHA1_HASH_LEN);
		}
		else if (!mIndexingInProgress)
		{
			AAMPLOG_INFO("%s:%d Same DRM Metadata", __FUNCTION__, __LINE__);
		}
	}
	else
	{
		if (!mIndexingInProgress)
		{
			printf("%s:%d [%s] New DRM metadata hash - ", __FUNCTION__, __LINE__, name);
			for (int i = 0; i < DRM_SHA1_HASH_LEN; i++)
			{
				printf("%c", ptr[i]);
			}
			printf("\n");
		}
		mCMSha1Hash = (char*)malloc(DRM_SHA1_HASH_LEN);
		memcpy(mCMSha1Hash, ptr, DRM_SHA1_HASH_LEN);
		drmDataChanged = true;
	}
	if(drmDataChanged)
	{
		int i;
		DrmMetadataNode* drmMetadataNode = (DrmMetadataNode*)mDrmMetaDataIndex.ptr;
		for (i = 0; i < mDrmMetaDataIndexCount; i++)
		{
			if(drmMetadataNode[i].sha1Hash)
			{
				if (0 == memcmp(mCMSha1Hash, drmMetadataNode[i].sha1Hash, DRM_SHA1_HASH_LEN))
				{
					if (!mIndexingInProgress)
					{
						AAMPLOG_INFO("%s:%d mDrmMetaDataIndexPosition %d->%d", __FUNCTION__, __LINE__, mDrmMetaDataIndexPosition, i);
					}
					mDrmMetaDataIndexPosition = i;
					break;
				}
			}
		}
		if (i == mDrmMetaDataIndexCount)
		{
			logprintf("%s:%d [%s] Couldn't find matching hash mDrmMetaDataIndexCount %d ", __FUNCTION__, __LINE__,
			        name, mDrmMetaDataIndexCount);
			for (int j = 0; j < mDrmMetaDataIndexCount; j++)
			{
				if (drmMetadataNode[j].sha1Hash)
				{
					printf("%s:%d drmMetadataNode[%d].sha1Hash -- \n", __FUNCTION__, __LINE__, j);
					for (int i = 0; i < DRM_SHA1_HASH_LEN; i++)
					{
						printf("%c", drmMetadataNode[j].sha1Hash[i]);
					}
					printf("\n");
				}
				else
				{
					logprintf("%s:%d drmMetadataNode[%d].sha1Hash NULL", __FUNCTION__, __LINE__, j);
				}
			}
			for (int i = 0; i < playlist.len; i++)
			{
				char temp = playlist.ptr[i];
				if (temp == '\0')
				{
					temp = '\n';
				}
				putchar(temp);
			}
			assert(false);
		}
	}
}
/***************************************************************************
* @fn UpdateDrmIV
* @brief Function to update IV from DRM 
*		 
* @param ptr[in] IV string from DRM attribute
* @return void
***************************************************************************/
void TrackState::UpdateDrmIV(const char *ptr)
{
	size_t len;
	unsigned char *iv = base16_Decode(ptr, (DRM_IV_LEN*2), &len); // 32 characters encoding 128 bits (16 bytes)
	assert(len == DRM_IV_LEN);
	if(mDrmInfo.iv)
	{
		if(0 != memcmp(mDrmInfo.iv, iv, DRM_IV_LEN))
		{
			traceprintf("%s:%d Different DRM IV - ", __FUNCTION__, __LINE__);
#ifdef TRACE
			for (int i = 0; i< DRM_IV_LEN*2; i++)
			{
				printf("%c", ptr[i]);
			}
			printf("\n");
#endif
		}
		else
		{
			traceprintf("%s:%d Same DRM IV", __FUNCTION__, __LINE__);
		}
		free(mDrmInfo.iv);
	}
	mDrmInfo.iv = iv;
	traceprintf("%s:%d [%s] Exit mDrmInfo.iv %p", __FUNCTION__, __LINE__, name, mDrmInfo.iv);
}
/***************************************************************************
* @fn FetchPlaylist
* @brief Function to fetch playlist file
*		 
* @return void
***************************************************************************/

void TrackState::FetchPlaylist()
{
	int playlistDownloadFailCount = 0;
	long http_error;
	ProfilerBucketType bucketId = (this->type == eTRACK_SUBTITLE)?PROFILE_BUCKET_PLAYLIST_SUBTITLE:(this->type == eTRACK_AUDIO)?PROFILE_BUCKET_PLAYLIST_AUDIO:PROFILE_BUCKET_PLAYLIST_VIDEO;
	logprintf("TrackState::%s [%s] start", __FUNCTION__, name);
	aamp->profiler.ProfileBegin(bucketId);
	do
	{
		int iCurrentRate = aamp->rate; //  Store it as back up, As sometimes by the time File is downloaded, rate might have changed due to user initiated Trick-Play
		MediaType mType = (this->type == eTRACK_SUBTITLE) ? eMEDIATYPE_PLAYLIST_SUBTITLE : (this->type == eTRACK_AUDIO) ? eMEDIATYPE_PLAYLIST_AUDIO : eMEDIATYPE_PLAYLIST_VIDEO;
		aamp->GetFile(mPlaylistUrl, &playlist, mEffectiveUrl, &http_error, NULL, type, true, mType);
		//update videoend info
		aamp->UpdateVideoEndMetrics( (IS_FOR_IFRAME(iCurrentRate,this->type) ? eMEDIATYPE_PLAYLIST_IFRAME :mType),(this->GetCurrentBandWidth()*8),
									http_error,mEffectiveUrl);
		if(playlist.len)
		{
			aamp->profiler.ProfileEnd(bucketId);
			break;
		}
		logprintf("Playlist download failed : %s failure count : %d : http response : %d", mPlaylistUrl.c_str(), playlistDownloadFailCount, (int)http_error);
		aamp->InterruptableMsSleep(500);
		playlistDownloadFailCount += 1;
	} while(aamp->DownloadsAreEnabled() && (MAX_MANIFEST_DOWNLOAD_RETRY >  playlistDownloadFailCount) && (404 == http_error));
	logprintf("TrackState::%s [%s] end", __FUNCTION__, name);
	if (!playlist.len)
	{
		aamp->profiler.ProfileError(bucketId, http_error);
	}
}


/***************************************************************************
* @fn GetBWIndex
* @brief Function to get bandwidth index corresponding to bitrate
*
* @param bitrate Bitrate in bits per second
* @return bandwidth index
***************************************************************************/
int StreamAbstractionAAMP_HLS::GetBWIndex(long bitrate)
{
	int topBWIndex = 0;
	int profileCount = GetProfileCount();
	if (profileCount)
	{
		for (int i = 0; i < profileCount; i++)
		{
			struct HlsStreamInfo *streamInfo = &this->streamInfo[i];
			if (!streamInfo->isIframeTrack && streamInfo->bandwidthBitsPerSecond > bitrate)
			{
				--topBWIndex;
			}
		}
	}
	return topBWIndex;
}

/***************************************************************************
* @fn GetNextFragmentPeriodInfo
* @brief Function to get next playback position from start, to handle discontinuity 
*		 
* @param periodIdx[out] Period Index 	
* @param offsetFromPeriodStart[out] Offset value
* @return void
***************************************************************************/
void TrackState::GetNextFragmentPeriodInfo(int &periodIdx, double &offsetFromPeriodStart)
{
	const IndexNode *index = (IndexNode *) this->index.ptr;
	const IndexNode *idxNode = NULL;
	periodIdx = -1;
	offsetFromPeriodStart = 0;
	int idx;
	double prevCompletionTimeSecondsFromStart = 0;
	assert(context->rate > 0);
	for (idx = 0; idx < indexCount; idx++)
	{
		const IndexNode *node = &index[idx];
		if (node->completionTimeSecondsFromStart > playTarget)
		{
			logprintf("%s (%s) Found node - rate %f completionTimeSecondsFromStart %f playTarget %f", __FUNCTION__, name,
			        context->rate, node->completionTimeSecondsFromStart, playTarget);
			idxNode = node;
			break;
		}
		prevCompletionTimeSecondsFromStart = node->completionTimeSecondsFromStart;
	}
	if (idxNode)
	{
		if (idx > 0)
		{
			offsetFromPeriodStart = prevCompletionTimeSecondsFromStart;
			double periodStartPosition = 0;
			DiscontinuityIndexNode* discontinuityIndex = (DiscontinuityIndexNode*)mDiscontinuityIndex.ptr;
			for (int i = 0; i < mDiscontinuityIndexCount; i++)
			{
				traceprintf("TrackState::%s [%s] Loop periodItr %d idx %d first %d second %f", __FUNCTION__, name, i, idx,
				        discontinuityIndex[i].fragmentIdx, discontinuityIndex[i].position);
				if (discontinuityIndex[i].fragmentIdx > idx)
				{
					logprintf("TrackState::%s [%s] Found periodItr %d idx %d first %d offsetFromPeriodStart %f",
					        __FUNCTION__, name, i, idx, discontinuityIndex[i].fragmentIdx, periodStartPosition);
					break;
				}
				periodIdx = i;
				periodStartPosition = discontinuityIndex[i].position;
			}
			offsetFromPeriodStart -= periodStartPosition;
		}
		logprintf("TrackState::%s [%s] periodIdx %d offsetFromPeriodStart %f", __FUNCTION__, name, periodIdx,
		        offsetFromPeriodStart);
	}
	else
	{
		logprintf("TrackState::%s [%s] idxNode NULL", __FUNCTION__, name);
	}
}
/***************************************************************************
* @fn GetPeriodStartPosition
* @brief Function to get Period start position for given period index,to handle discontinuity
*		 
* @param periodIdx[in] Period Index 
* @return void
***************************************************************************/
double TrackState::GetPeriodStartPosition(int periodIdx)
{
	double offset = 0;
	logprintf("TrackState::%s [%s] periodIdx %d periodCount %d", __FUNCTION__, name, periodIdx,
	        (int) mDiscontinuityIndexCount);
	if (periodIdx < mDiscontinuityIndexCount)
	{
		int count = 0;
		DiscontinuityIndexNode* discontinuityIndex = (DiscontinuityIndexNode*)mDiscontinuityIndex.ptr;
		for (int i = 0; i < mDiscontinuityIndexCount; i++)
		{
			if (count == periodIdx)
			{
				offset = discontinuityIndex[i].position;
				logprintf("TrackState::%s [%s] offset %f periodCount %d", __FUNCTION__, name, offset,
				        (int) mDiscontinuityIndexCount);
				break;
			}
			else
			{
				count++;
			}
		}
	}
	else
	{
		logprintf("TrackState::%s [%s] WARNING periodIdx %d periodCount %d", __FUNCTION__, name, periodIdx,
		        mDiscontinuityIndexCount);
	}
	return offset;
}
/***************************************************************************
* @fn GetNumberOfPeriods
* @brief Function to return number of periods stored in playlist
*		 
* @return int number of periods
***************************************************************************/
int TrackState::GetNumberOfPeriods()
{
	return mDiscontinuityIndexCount;
}

/***************************************************************************
* @fn HasDiscontinuityAroundPosition
* @brief Check if discontinuity present around given position
* @param[in] position Position to check for discontinuity
* @param[out] diffBetweenDiscontinuities discontinuity position minus input position
* @return true if discontinuity present around given position
***************************************************************************/
bool TrackState::HasDiscontinuityAroundPosition(double position, bool useStartTime, double &diffBetweenDiscontinuities, double playPosition)
{
	bool discontinuityPending = false;
	double low = position - DISCONTINUITY_DISCARD_TOLERANCE_SECONDS;
	double high = position + DISCONTINUITY_DISCARD_TOLERANCE_SECONDS;
	int playlistRefreshCount = 0;
	diffBetweenDiscontinuities = DBL_MAX;
	pthread_mutex_lock(&mPlaylistMutex);
	while (aamp->DownloadsAreEnabled())
	{
		if (0 != mDiscontinuityIndexCount)
		{
			DiscontinuityIndexNode* discontinuityIndex = (DiscontinuityIndexNode*)mDiscontinuityIndex.ptr;
			for (int i = 0; i < mDiscontinuityIndexCount; i++)
			{
				if ((mLastMatchedDiscontPosition < 0)
						|| (discontinuityIndex[i].position + mCulledSeconds > mLastMatchedDiscontPosition))
				{
					if (!useStartTime)
					{
						traceprintf ("%s:%d low %f high %f position %f discontinuity %f", __FUNCTION__, __LINE__, low, high, position, discontinuityIndex[i].position);
						if (low < discontinuityIndex[i].position && high > discontinuityIndex[i].position)
						{
							mLastMatchedDiscontPosition = discontinuityIndex[i].position + mCulledSeconds;
							discontinuityPending = true;
							break;
						}
					}
					else
					{
						struct timeval programDateTimeVal;
						if (ParseTimeFromProgramDateTime(discontinuityIndex[i].programDateTime, programDateTimeVal))
						{
							double discPos = programDateTimeVal.tv_sec + (double) programDateTimeVal.tv_usec/1000000;
							logprintf ("%s:%d low %f high %f position %f discontinuity %f", __FUNCTION__, __LINE__, low, high, position, discPos);

							if (low < discPos && high > discPos)
							{
								double diff = discPos - position;
								discontinuityPending = true;
								if (fabs(diff) < fabs(diffBetweenDiscontinuities))
								{
									diffBetweenDiscontinuities = diff;
									mLastMatchedDiscontPosition = discontinuityIndex[i].position + mCulledSeconds;
								}
								else
								{
									break;
								}
							}
						}
					}
				}
			}
		}
		if (!discontinuityPending)
		{
			logprintf("%s:%d ##[%s] Discontinuity not found in window low %f high %f position %f mLastMatchedDiscontPosition %f mDuration %f playPosition %f playlistRefreshCount %d playlistType %d useStartTime %d", __FUNCTION__, __LINE__,
					name, low, high, position, mLastMatchedDiscontPosition, mDuration, playPosition, playlistRefreshCount, (int)mPlaylistType, (int)useStartTime);
			if (IsLive())
			{
				int maxPlaylistRefreshCount;
				bool liveNoTSB;
				if (aamp->IsTSBSupported() || aamp->IsInProgressCDVR())
				{
					maxPlaylistRefreshCount = MAX_PLAYLIST_REFRESH_FOR_DISCONTINUITY_CHECK_EVENT;
					liveNoTSB = false;
				}
				else
				{
					maxPlaylistRefreshCount = MAX_PLAYLIST_REFRESH_FOR_DISCONTINUITY_CHECK_LIVE;
					liveNoTSB = true;
				}
				if ((playlistRefreshCount < maxPlaylistRefreshCount)
						&& (liveNoTSB || (mDuration < (playPosition + DISCONTINUITY_DISCARD_TOLERANCE_SECONDS))))
				{
					logprintf("%s:%d Waiting for %s playlist update mDuration %f mCulledSeconds %f", __FUNCTION__,
					        __LINE__, name, mDuration, mCulledSeconds);
					pthread_cond_wait(&mPlaylistIndexed, &mPlaylistMutex);
					logprintf("%s:%d Wait for %s playlist update over", __FUNCTION__, __LINE__, name);
					playlistRefreshCount++;
				}
				else
				{
					break;
				}
			}
			else
			{
				break;
			}
		}
		else
		{
			break;
		}
	}
	pthread_mutex_unlock(&mPlaylistMutex);
	return discontinuityPending;
}

/***************************************************************************
* @fn FetchInitFragment
* @brief Function to fetch init fragment
*
* @return void
***************************************************************************/
void TrackState::FetchInitFragment()
{
	int timeoutMs = -1;

	if (IsLive())
	{
		timeoutMs = context->maxIntervalBtwPlaylistUpdateMs - (int) (aamp_GetCurrentTimeMS() - lastPlaylistDownloadTimeMS);
		if(timeoutMs < 0)
		{
			timeoutMs = 0;
		}
	}
	if (mInjectInitFragment && mInitFragmentInfo)
	{
		if (!WaitForFreeFragmentAvailable(timeoutMs))
		{
			return;
		}

		long http_code = -1;
		bool forcePushEncryptedHeader = (!fragmentEncrypted && mCheckForInitialFragEnc);
		// Check if we have encrypted header successfully parsed to push ahead
		if (forcePushEncryptedHeader && mFirstEncInitFragmentInfo == NULL)
		{
			AAMPLOG_WARN("TrackState::%s:%d [%s] first encrypted init-fragment is NULL! fragmentEncrypted-%d", __FUNCTION__, __LINE__, name, fragmentEncrypted);
			forcePushEncryptedHeader = false;
		}

		ProfilerBucketType bucketType = aamp->GetProfilerBucketForMedia((MediaType)type, true);
		aamp->profiler.ProfileBegin(bucketType);
		if(FetchInitFragmentHelper(http_code, forcePushEncryptedHeader))
		{
			aamp->profiler.ProfileEnd(bucketType);

			CachedFragment* cachedFragment = GetFetchBuffer(false);
			if (cachedFragment->fragment.ptr)
			{
				cachedFragment->duration = 0;
				cachedFragment->position = playTarget - playTargetOffset;
				cachedFragment->discontinuity = discontinuity;
			}

			// If forcePushEncryptedHeader, don't reset the playTarget as the original init header has to be pushed next
			if (!forcePushEncryptedHeader)
			{
				mInjectInitFragment = false;
			}

			discontinuity = false; //reset discontinuity which has been set for init fragment now
			mSkipAbr = true; //Skip ABR, since last fragment cached is init fragment.
			mCheckForInitialFragEnc = false; //Push encrypted header is a one-time operation

			UpdateTSAfterFetch();
		}
		else if (aamp->DownloadsAreEnabled())
		{
			AAMPLOG_ERR("TrackState::%s:%d Init fragment fetch failed", __FUNCTION__, __LINE__);
			aamp->profiler.ProfileError(bucketType, http_code);
			aamp->SendDownloadErrorEvent(AAMP_TUNE_INIT_FRAGMENT_DOWNLOAD_FAILURE, http_code);
		}
	}
	else if (!mInitFragmentInfo)
	{
		AAMPLOG_ERR("TrackState::%s:%d Need to push init fragment but fragment info is missing! mInjectInitFragment(%d)", __FUNCTION__, __LINE__, mInjectInitFragment);
		mInjectInitFragment = false;
	}

}

/***************************************************************************
* @brief Helper to fetch init fragment for fragmented mp4 format
* @return true if success
***************************************************************************/
bool TrackState::FetchInitFragmentHelper(long &http_code, bool forcePushEncryptedHeader)
{
	bool ret = false;
	std::istringstream initFragmentUrlStream;
	traceprintf("%s:%d Enter", __FUNCTION__, __LINE__);

	// If the first init fragment is of a clear fragment, we push an encrypted fragment's
	// init data first to let qtdemux know we will need decryptor plugins
	AAMPLOG_TRACE("TrackState::%s:%d [%s] fragmentEncrypted-%d mFirstEncInitFragmentInfo-%s", __FUNCTION__, __LINE__, name, fragmentEncrypted, mFirstEncInitFragmentInfo);
	if (forcePushEncryptedHeader)
	{
		//Push encrypted fragment's init data first
		AAMPLOG_WARN("TrackState::%s:%d [%s] first init-fragment is unencrypted.! Pushing encrypted init-header", __FUNCTION__, __LINE__, name);
		initFragmentUrlStream = std::istringstream(std::string(mFirstEncInitFragmentInfo));
	}
	else
	{
		initFragmentUrlStream = std::istringstream(std::string(mInitFragmentInfo));
	}
	std::string line;
	std::getline(initFragmentUrlStream, line);
	if (!line.empty())
	{
		const char *range = NULL;
		char rangeStr[128];
		std::string uri;
		traceprintf("%s:%d line %s", __FUNCTION__, __LINE__, line.c_str());
		size_t uriTagStart = line.find("URI=");
		if (uriTagStart != std::string::npos)
		{
			std::string uriStart = line.substr(uriTagStart + 5);
			traceprintf("%s:%d uriStart %s", __FUNCTION__, __LINE__, uriStart.c_str());
			size_t uriTagEnd = uriStart.find("\"");
			if (uriTagEnd != std::string::npos)
			{
				traceprintf("%s:%d uriTagEnd %d", __FUNCTION__, __LINE__, (int) uriTagEnd);
				uri = uriStart.substr(0, uriTagEnd);
				traceprintf("%s:%d uri %s", __FUNCTION__, __LINE__, uri.c_str());
			}
			else
			{
				AAMPLOG_ERR("%s:%d URI parse error. Tag end not found", __FUNCTION__, __LINE__);
			}
		}
		else
		{
			AAMPLOG_ERR("%s:%d URI parse error. URI= not found", __FUNCTION__, __LINE__);
		}
		size_t byteRangeTagStart = line.find("BYTERANGE=");
		if (byteRangeTagStart != std::string::npos)
		{
			std::string byteRangeStart = line.substr(byteRangeTagStart + 11);
			size_t byteRangeTagEnd = byteRangeStart.find("\"");
			if (byteRangeTagEnd != std::string::npos)
			{
				std::string byteRange = byteRangeStart.substr(0, byteRangeTagEnd);
				traceprintf("%s:%d byteRange %s", __FUNCTION__, __LINE__, byteRange.c_str());
				if (!byteRange.empty())
				{
					size_t offsetIdx = byteRange.find("@");
					if (offsetIdx != std::string::npos)
					{
						int offsetVal = stoi(byteRange.substr(offsetIdx + 1));
						int rangeVal = stoi(byteRange.substr(0, offsetIdx));
						int next = offsetVal + rangeVal;
						sprintf(rangeStr, "%d-%d", offsetVal, next - 1);
						AAMPLOG_INFO("TrackState::%s:%d rangeStr %s", __FUNCTION__, __LINE__, rangeStr);
						range = rangeStr;
					}
				}
			}
			else
			{
				AAMPLOG_ERR("TrackState::%s:%d byteRange parse error. Tag end not found byteRangeStart %s",
						__FUNCTION__, __LINE__, byteRangeStart.c_str());
			}
		}
		if (!uri.empty())
		{
			std::string fragmentUrl;
			aamp_ResolveURL(fragmentUrl, mEffectiveUrl, uri.c_str());
			std::string tempEffectiveUrl;
			CachedFragment* cachedFragment = GetFetchBuffer(true);
			AAMPLOG_WARN("TrackState::%s:%d [%s] init-fragment = %s", __FUNCTION__, __LINE__, name, fragmentUrl.c_str());
			int iCurrentRate = aamp->rate; //  Store it as back up, As sometimes by the time File is downloaded, rate might have changed due to user initiated Trick-Play
			bool fetched = aamp->GetFile(fragmentUrl, &cachedFragment->fragment, tempEffectiveUrl, &http_code, range,
			        type, false, (MediaType) (type));

			MediaType actualType = eMEDIATYPE_INIT_VIDEO ;
			if(IS_FOR_IFRAME(iCurrentRate,type))
			{
				actualType = eMEDIATYPE_INIT_IFRAME;
			}
			else if (type == eTRACK_AUDIO )
			{
				actualType = eMEDIATYPE_INIT_AUDIO ;
			}

			aamp->UpdateVideoEndMetrics(actualType, (this->GetCurrentBandWidth() *8), http_code, mEffectiveUrl);

			if (!fetched)
			{
				AAMPLOG_ERR("TrackState::%s:%d aamp_GetFile failed", __FUNCTION__, __LINE__);
				aamp_Free(&cachedFragment->fragment.ptr);
			}
			else
			{
				ret = true;
			}
		}
		else
		{
			AAMPLOG_ERR("TrackState::%s:%d Could not parse init fragment URI. line %s", __FUNCTION__, __LINE__, line.c_str());
		}
	}
	else
	{
		AAMPLOG_ERR("TrackState::%s:%d Init fragment URI parse error", __FUNCTION__, __LINE__);
	}
	return ret;
}

/***************************************************************************
* @fn StopInjection
* @brief Function to stop fragment injection
*
* @return void
***************************************************************************/
void StreamAbstractionAAMP_HLS::StopInjection(void)
{
	//invoked at times of discontinuity. Audio injection loop might have already exited here
	ReassessAndResumeAudioTrack(true);

	for (int iTrack = 0; iTrack < AAMP_TRACK_COUNT; iTrack++)
	{
		TrackState *track = trackState[iTrack];
		if(track && track->Enabled())
		{
			track->StopInjection();
		}
	}
}

/***************************************************************************
* @fn StopInjection
* @brief Function to stop fragment injection
*
* @return void
***************************************************************************/
void TrackState::StopInjection()
{
	AbortWaitForCachedFragment();
	aamp->StopTrackInjection((MediaType) type);
	if (playContext)
	{
		playContext->abort();
	}
	StopInjectLoop();
}

/***************************************************************************
* @fn StartInjection
* @brief starts fragment injection
*
* @return void
***************************************************************************/
void TrackState::StartInjection()
{
	aamp->ResumeTrackInjection((MediaType) type);
	if (playContext)
	{
		playContext->reset();
	}
	StartInjectLoop();
}

/***************************************************************************
* @fn StartInjection
* @brief Function to start fragment injection
*
* @return void
***************************************************************************/
void StreamAbstractionAAMP_HLS::StartInjection(void)
{
	for (int iTrack = 0; iTrack < AAMP_TRACK_COUNT; iTrack++)
	{
		TrackState *track = trackState[iTrack];
		if(track && track->Enabled())
		{
			track->StartInjection();
		}
	}
}

/***************************************************************************
* @fn StopWaitForPlaylistRefresh
* @brief Stop wait for playlist refresh
*
* @return void
***************************************************************************/
void TrackState::StopWaitForPlaylistRefresh()
{
	logprintf("%s:%d track [%s]", __FUNCTION__, __LINE__, name);
	pthread_mutex_lock(&mPlaylistMutex);
	pthread_cond_signal(&mPlaylistIndexed);
	pthread_mutex_unlock(&mPlaylistMutex);
}

/***************************************************************************
* @fn CancelDrmOperation
* @brief Cancel all DRM operations
*
* @param[in] clearDRM flag indicating if DRM resources to be freed or not
* @return void
***************************************************************************/
void TrackState::CancelDrmOperation(bool clearDRM)
{
	//Calling mDrm is required for AES encrypted assets which doesn't have AveDrmManager
	if (mDrm)
	{
		//To force release mTrackDrmMutex mutex held by drm_Decrypt in case of clearDRM
		mDrm->CancelKeyWait();
		if (clearDRM && aamp->GetCurrentDRM() != eDRM_Adobe_Access)
		{
			pthread_mutex_lock(&mTrackDrmMutex);
			mDrm->Release();
			pthread_mutex_unlock(&mTrackDrmMutex);
		}
	}
}

/***************************************************************************
* @fn RestoreDrmState
* @brief Restore DRM states
*
* @return void
***************************************************************************/
void TrackState::RestoreDrmState()
{
	if (mDrm)
	{
		mDrm->RestoreKeyState();
	}
}

/***************************************************************************
* @fn FindTimedMetadata
* @brief Function to search playlist for subscribed tags
*
* @return void
***************************************************************************/
void TrackState::FindTimedMetadata()
{
	double totalDuration = 0.0;
	traceprintf("%s:%d Enter", __FUNCTION__, __LINE__);
	if (gpGlobalConfig->enableSubscribedTags && (eTRACK_VIDEO == type))
	{
		pthread_mutex_lock(&mPlaylistMutex);
		if (playlist.ptr)
		{
			char *ptr = GetNextLineStart(playlist.ptr);
			while (ptr)
			{
				if(startswith(&ptr,"#EXT"))
				{
					if (startswith(&ptr, "INF:"))
					{
						totalDuration += atof(ptr);
					}
					for (int i = 0; i < aamp->subscribedTags.size(); i++)
					{
						const char* data = aamp->subscribedTags.at(i).data();
						if(startswith(&ptr, (data + 4))) // remove the TAG and only keep value(content) in PTR
						{
							ptr++; // skip the ":"
							int nb = (int)FindLineLength(ptr);
							//logprintf("[AAMP_JS] Found subscribedTag[%d]: @%f '%.*s'", i, totalDuration, nb, ptr);
							aamp->ReportTimedMetadata((mCulledSeconds + totalDuration) * 1000, data, ptr, nb);
							break;
						}
					}
				}
				ptr=GetNextLineStart(ptr);
			}
		}
		pthread_mutex_unlock(&mPlaylistMutex);
	}
	traceprintf("%s:%d Exit", __FUNCTION__, __LINE__);
}

/**
 * @}
 */


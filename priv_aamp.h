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
 * @file priv_aamp.h
 * @brief Private functions and types used internally by AAMP
 */

#ifndef PRIVAAMP_H
#define PRIVAAMP_H

#include "AampMemoryUtils.h"
#include "AampProfiler.h"
#include "AampDrmHelper.h"
#include "AampDrmMediaFormat.h"
#include "AampDrmCallbacks.h"
#include "GlobalConfigAAMP.h"
#include "main_aamp.h"
#ifdef SESSION_STATS
#include <IPVideoStat.h>
#endif

#include <pthread.h>
#include <signal.h>
#include <semaphore.h>
#include <curl/curl.h>
#include <string.h> // for memset
#include <vector>
#include <unordered_map>
#include <map>
#include <set>
#include <list>
#include <sstream>
#include <mutex>
#include <queue>
#include <algorithm>
#include <glib.h>
#include <cjson/cJSON.h>

static const char *mMediaFormatName[] =
{
    "HLS","DASH","PROGRESSIVE","HLS_MP4","OTA","HDMI_IN","COMPOSITE_IN","UNKNOWN"
};

#ifdef __APPLE__
#define aamp_pthread_setname(tid,name) pthread_setname_np(name)
#else
#define aamp_pthread_setname(tid,name) pthread_setname_np(tid,name)
#endif

#define AAMP_TRACK_COUNT 3              /**< internal use - audio+video+sub track */
#define DEFAULT_CURL_INSTANCE_COUNT (AAMP_TRACK_COUNT + 1) // One for Manifest/Playlist + Number of tracks
#define AAMP_DRM_CURL_COUNT 2           /**< audio+video track DRMs */
#define AAMP_LIVE_OFFSET 15             /**< Live offset in seconds */
#define AAMP_CDVR_LIVE_OFFSET 30 	/**< Live offset in seconds for CDVR hot recording */
#define CURL_FRAGMENT_DL_TIMEOUT 10L    /**< Curl timeout for fragment download */
#define DEFAULT_PLAYLIST_DL_TIMEOUT 10L /**< Curl timeout for playlist download */
#define DEFAULT_CURL_TIMEOUT 5L         /**< Default timeout for Curl downloads */
#define DEFAULT_CURL_CONNECTTIMEOUT 3L  /**< Curl socket connection timeout */
#define EAS_CURL_TIMEOUT 3L             /**< Curl timeout for EAS manifest downloads */
#define EAS_CURL_CONNECTTIMEOUT 2L      /**< Curl timeout for EAS connection */
#define DEFAULT_INTERVAL_BETWEEN_PLAYLIST_UPDATES_MS (6*1000)   /**< Interval between playlist refreshes */

#define AAMP_SEEK_TO_LIVE_POSITION (-1)

#define MANIFEST_TEMP_DATA_LENGTH 100				/**< Manifest temp data length */
#define AAMP_LOW_BUFFER_BEFORE_RAMPDOWN 10 // 10sec buffer before rampdown
#define AAMP_HIGH_BUFFER_BEFORE_RAMPUP  15 // 15sec buffer before rampup


#define AAMP_USER_AGENT_MAX_CONFIG_LEN  512    /**< Max Chars allowed in aamp.cfg for user-agent */

// MSO-specific VSS Service Zone identifier in URL
#define VSS_MARKER			"?sz="
#define VSS_MARKER_LEN			4
#define VSS_MARKER_FOG			"%3Fsz%3D" // URI-encoded ?sz=
#define VSS_VIRTUAL_STREAM_ID_KEY_STR "content:xcal:virtualStreamId"
#define VSS_VIRTUAL_STREAM_ID_PREFIX "urn:merlin:linear:stream:"
#define VSS_SERVICE_ZONE_KEY_STR "device:xcal:serviceZone"

/*1 for debugging video track, 2 for audio track, 4 for subtitle track and 7 for all*/
/*#define AAMP_DEBUG_FETCH_INJECT 0x001 */

/**
 * @brief Max debug log buffer size
 */
#define MAX_DEBUG_LOG_BUFF_SIZE 1024

/**
 * @brief Max URL log size
 */
#define MAX_URL_LOG_SIZE 960	// Considering "aamp_tune" and [AAMP-PLAYER] pretext

#define CONVERT_SEC_TO_MS(_x_) (_x_ * 1000) /**< Convert value to sec to ms*/
#define DEFAULT_PREBUFFER_COUNT (2)
#define DEFAULT_PRECACHE_WINDOW (10) 	// 10 mins for full precaching
#define DEFAULT_DOWNLOAD_RETRY_COUNT (1)		// max download failure retry attempt count
// These error codes are used internally to identify the cause of error from GetFile
#define PARTIAL_FILE_CONNECTIVITY_AAMP (130)
#define PARTIAL_FILE_DOWNLOAD_TIME_EXPIRED_AAMP (131)
#define OPERATION_TIMEOUT_CONNECTIVITY_AAMP (132)
#define PARTIAL_FILE_START_STALL_TIMEOUT_AAMP (133)
#define AAMP_MINIMUM_AUDIO_LEVEL (0) /**< minimum value for audio level supported */
#define AAMP_MAXIMUM_AUDIO_LEVEL (100) /**< maximum value for audio level supported */

#define STRBGPLAYER "BACKGROUND"
#define STRFGPLAYER "FOREGROUND"

/**
 * @brief Structure of X-Start HLS Tag
 */
struct HLSXStart
{
	double offset;      /**< Time offset from XStart */
	bool precise;     	/**< Precise input */
};

/**
 * @addtogroup AAMP_COMMON_TYPES
 * @{
 */

/**
 * @brief Enumeration for Curl Instances
 */
enum AampCurlInstance
{
	eCURLINSTANCE_VIDEO,
	eCURLINSTANCE_AUDIO,
	eCURLINSTANCE_SUBTITLE,
	eCURLINSTANCE_MANIFEST_PLAYLIST,
	eCURLINSTANCE_DAI,
	eCURLINSTANCE_AES,
	eCURLINSTANCE_PLAYLISTPRECACHE,
	eCURLINSTANCE_MAX
};

/*
 * @brief Playback Error Type
 */
enum PlaybackErrorType
{
	eGST_ERROR_PTS,                 /**< PTS error from gstreamer */
	eGST_ERROR_UNDERFLOW,           /**< Underflow error from gstreamer */
	eGST_ERROR_VIDEO_BUFFERING,     /**< Video buffering error */
	eGST_ERROR_OUTPUT_PROTECTION_ERROR,     /**< Output Protection error */
	eDASH_ERROR_STARTTIME_RESET,    /**< Start time reset of DASH */
	eSTALL_AFTER_DISCONTINUITY,		/** Playback stall after notifying discontinuity */
	eGST_ERROR_GST_PIPELINE_INTERNAL	/** GstPipeline Internal Error */
};


/**
 * @brief Tune Typea
 */
enum TuneType
{
	eTUNETYPE_NEW_NORMAL,   /**< Play from live point for live streams, from start for VOD*/
	eTUNETYPE_NEW_SEEK,     /**< A new tune with valid seek position*/
	eTUNETYPE_SEEK,         /**< Seek to a position. Not a new channel, so resources can be reused*/
	eTUNETYPE_SEEKTOLIVE,   /**< Seek to live point. Not a new channel, so resources can be reused*/
	eTUNETYPE_RETUNE,       /**< Internal retune for error handling.*/
	eTUNETYPE_LAST          /**< Use the tune mode used in last tune*/
};

/**
 * @brief AAMP Function return values
 */
enum AAMPStatusType
{
	eAAMPSTATUS_OK,
	eAAMPSTATUS_GENERIC_ERROR,
	eAAMPSTATUS_MANIFEST_DOWNLOAD_ERROR,
	eAAMPSTATUS_PLAYLIST_VIDEO_DOWNLOAD_ERROR,
	eAAMPSTATUS_PLAYLIST_AUDIO_DOWNLOAD_ERROR,
	eAAMPSTATUS_MANIFEST_PARSE_ERROR,
	eAAMPSTATUS_MANIFEST_CONTENT_ERROR,
	eAAMPSTATUS_MANIFEST_INVALID_TYPE,
	eAAMPSTATUS_PLAYLIST_PLAYBACK,
	eAAMPSTATUS_SEEK_RANGE_ERROR,
	eAAMPSTATUS_TRACKS_SYNCHRONISATION_ERROR,
	eAAMPSTATUS_UNSUPPORTED_DRM_ERROR
};


/**
 * @brief Http Header Type
 */
enum HttpHeaderType
{
	eHTTPHEADERTYPE_COOKIE,     /**< Cookie Header */
	eHTTPHEADERTYPE_XREASON,    /**< X-Reason Header */
	eHTTPHEADERTYPE_FOG_REASON, /**< X-Reason Header */
	eHTTPHEADERTYPE_EFF_LOCATION, /**< Effective URL location returned */
	eHTTPHEADERTYPE_UNKNOWN=-1  /**< Unkown Header */
};


/**
 * @brief Http Header Type
 */
enum CurlAbortReason
{
	eCURL_ABORT_REASON_NONE = 0,
	eCURL_ABORT_REASON_STALL_TIMEDOUT,
	eCURL_ABORT_REASON_START_TIMEDOUT
};

/**
 * @brief Different reasons for bitrate change
 */
typedef enum
{
	eAAMP_BITRATE_CHANGE_BY_ABR = 0,
	eAAMP_BITRATE_CHANGE_BY_RAMPDOWN = 1,
	eAAMP_BITRATE_CHANGE_BY_TUNE = 2,
	eAAMP_BITRATE_CHANGE_BY_SEEK = 3,
	eAAMP_BITRATE_CHANGE_BY_TRICKPLAY = 4,
	eAAMP_BITRATE_CHANGE_BY_BUFFER_FULL = 5,
	eAAMP_BITRATE_CHANGE_BY_BUFFER_EMPTY = 6,
	eAAMP_BITRATE_CHANGE_BY_FOG_ABR = 7,
	eAAMP_BITRATE_CHANGE_MAX = 8
} BitrateChangeReason;

/**
 * @enum AudioType
 *
 * @brief Type of audio ES for MPD
 */
enum AudioType
{
	eAUDIO_UNSUPPORTED,
	eAUDIO_UNKNOWN,
	eAUDIO_AAC,
	eAUDIO_DDPLUS,
	eAUDIO_ATMOS
};

/**
 * @struct AsyncEventDescriptor
 * @brief Used in asynchronous event notification logic
 */
struct AsyncEventDescriptor
{
	/**
	 * @brief AsyncEventDescriptor constructor
	 */
	AsyncEventDescriptor() : event(nullptr), aamp(NULL)
	{
	}

	/**
	 * @brief AsyncEventDescriptor destructor
	 */
	virtual ~AsyncEventDescriptor()
	{
	}

	AsyncEventDescriptor(const AsyncEventDescriptor &other) = delete;
	AsyncEventDescriptor& operator=(const AsyncEventDescriptor& other) = delete;

	AAMPEventPtr event;
	PrivateInstanceAAMP* aamp;
};

/**
 * @brief Class for Timed Metadata
 */
class TimedMetadata
{
public:

	/**
	 * @brief TimedMetadata Constructor
	 */
	TimedMetadata() : _timeMS(0), _name(""), _content(""), _id(""), _durationMS(0) {}

	/**
	 * @brief TimedMetadata Constructor
	 *
	 * @param[in] timeMS - Time in milliseconds
	 * @param[in] name - Metadata name
	 * @param[in] content - Metadata content
	 */
	TimedMetadata(long long timeMS, std::string name, std::string content, std::string id, double durMS) : _timeMS(timeMS), _name(name), _content(content), _id(id), _durationMS(durMS) {}

public:
	long long _timeMS;     /**< Time in milliseconds */
	std::string _name;       /**< Metadata name */
	std::string _content;    /**< Metadata content */
	std::string _id;         /**< Id of the timedMetadata. If not available an Id will bre created */
	double      _durationMS; /**< Duration in milliseconds */
};


/**
 * @brief Function pointer for the idle task
 * @param[in] arg - Arguments
 * @return Idle task status
 */
typedef int(*IdleTask)(void* arg);

/**
 * @brief Function pointer for the destroy task
 *
 * @param[in] arg - Arguments
 *
 */
typedef void(*DestroyTask)(void * arg);

/**
 * @brief To store Set Cookie: headers and X-Reason headers in HTTP Response
 */
struct httpRespHeaderData {
	httpRespHeaderData() : type(0), data("")
	{
	}
	int type;             /**< Header type */
	std::string data;     /**< Header value */
};


/**
 * @brief  Structure of the event listener list
 */
struct ListenerData {
	EventListener* eventListener;   /**< Event listener */
	ListenerData* pNext;                /**< Next listener */
};


#ifdef AAMP_HLS_DRM
/**
 *	\Class attrNameData
 * 	\brief	local calss to hold DRM information
 */
class attrNameData
{
public:
	std::string attrName;
	bool isProcessed;
	attrNameData() : attrName(""),isProcessed(false)
	{
	}
		
	attrNameData(std::string argument) : attrName(argument), isProcessed(false)
	{
	}

	bool operator==(const attrNameData& rhs) const { return (this->attrName == rhs.attrName); }
};

#endif
/**
 * @}
 */

class AampCacheHandler;

class AampDRMSessionManager;

/**
 * @brief Class representing the AAMP player's private instance, which is not exposed to outside world.
 */
class PrivateInstanceAAMP : public AampDrmCallbacks
{

	enum AAMP2ReceiverMsgType
	{
	    E_AAMP2Receiver_TUNETIME,
	    E_AAMP2Receiver_EVENTS,
	    E_AAMP2Receiver_MsgMAX
	};
#ifdef WIN32
	// 'packed' directive unavailable in win32 build;
	typedef struct _AAMP2ReceiverMsg
#else
	// needed to ensure matching structure alignment in receiver
	typedef struct __attribute__((__packed__)) _AAMP2ReceiverMsg
#endif
	{
	    unsigned int type;
	    unsigned int length;
	    char data[1];
	}AAMP2ReceiverMsg;

	#define AAMP2ReceiverMsgHdrSz (sizeof(AAMP2ReceiverMsg)-1)

public:
	/**
	 * @brief Get profiler bucket type
	 *
	 * @param[in] mediaType - Media type. eg: Video, Audio, etc
	 * @param[in] isInitializationSegment - Initialization segment or not
	 * @return Bucket type
	 */
	ProfilerBucketType GetProfilerBucketForMedia(MediaType mediaType, bool isInitializationSegment)
	{
		switch (mediaType)
		{
		case eMEDIATYPE_SUBTITLE:
			return isInitializationSegment ? PROFILE_BUCKET_INIT_SUBTITLE : PROFILE_BUCKET_FRAGMENT_SUBTITLE;
		case eMEDIATYPE_VIDEO:
			return isInitializationSegment ? PROFILE_BUCKET_INIT_VIDEO : PROFILE_BUCKET_FRAGMENT_VIDEO;
		case eMEDIATYPE_AUDIO:
		default:
			return isInitializationSegment ? PROFILE_BUCKET_INIT_AUDIO : PROFILE_BUCKET_FRAGMENT_AUDIO;
		}
	}

	/**
	 * @brief Tune API
	 *
	 * @param[in] url - Asset URL
	 * @param[in] autoPlay - Start playback immediately or not
	 * @param[in] contentType - Content Type
	 * @param[in] bFirstAttempt - External initiated tune
	 * @param[in] bFinalAttempt - Final retry/attempt.
	 * @param[in] audioDecoderStreamSync - Enable or disable audio decoder stream sync,
	 *                set to 'false' if audio fragments come with additional padding at the end (BCOM-4203)
	 * @return void
	 */
	void Tune(const char *url, bool autoPlay,  const char *contentType = NULL, bool bFirstAttempt = true, bool bFinalAttempt = false, const char *sessionUUID = NULL,bool audioDecoderStreamSync = true);

	/**
	 * @brief The helper function which perform tuning
	 *
	 * @param[in] tuneType - Type of tuning. eg: Normal, trick, seek to live, etc
	 * @param[in] seekWhilePaused - Set true if want to keep in Paused state after
	 *              seek for tuneType = eTUNETYPE_SEEK or eTUNETYPE_SEEKTOLIVE
	 * @return void
	 */
	void TuneHelper(TuneType tuneType, bool seekWhilePaused = false);

	/**
	 * @brief Terminate the stream
	 *
	 * @param[in] newTune - New tune or not
	 * @return void
	 */
	void TeardownStream(bool newTune);

	/**
	 * @brief Send messages to Receiver over PIPE
	 *
	 * @param[in] str - Pointer to the message
	 * @param[in] nToWrite - Number of bytes in the message
	 * @return void
	 */
	void SendMessageOverPipe(const char *str,int nToWrite);

	/**
	 * @brief Establish PIPE session with Receiver
	 *
	 * @return Success/Failure
	 */
	bool SetupPipeSession();

	/**
	 * @brief Close PIPE session with Receiver
	 *
	 * @return void
	 */
	void ClosePipeSession();

	/**
	 * @brief Send message to reciever over PIPE
	 *
	 * @param[in] type - Message type
	 * @param[in] data - Message data
	 * @return void
	 */
	void SendMessage2Receiver(AAMP2ReceiverMsgType type, const char *data);

	/**
	 * @brief Send tune events to receiver
	 *
	 * @param[in] success - Tune status
	 * @return void
	 */
	void sendTuneMetrics(bool success);

	/**
	 * @brief To pause/play the gstreamer pipeline
	 *
	 * @param[in] success - true for pause and false for play
	 * @param[in] forceStopGstreamerPreBuffering - true for disabling bufferinprogress
	 * @return true on success
	 */
	bool PausePipeline(bool pause, bool forceStopGstreamerPreBuffering);

	/**
	 * @brief Convert media file type to profiler bucket type
	 *
	 * @param[in] fileType - Media filetype
	 * @return Profiler bucket type
	 */
	ProfilerBucketType mediaType2Bucket(MediaType fileType);

       /**
         * @brief to set the vod-tune-event according to the player
         *
         * @param[in] tuneEventType
         * @return void
         */
	void SetTuneEventConfig( TunedEventConfig tuneEventType);

	std::vector< std::pair<long long,long> > mAbrBitrateData;

	pthread_mutex_t mLock;// = PTHREAD_MUTEX_INITIALIZER;
	pthread_mutexattr_t mMutexAttr;
	pthread_mutex_t mParallelPlaylistFetchLock; // mutex lock for parallel fetch

	class StreamAbstractionAAMP *mpStreamAbstractionAAMP; // HLS or MPD collector
	class CDAIObject *mCdaiObject;      // Client Side DAI Object
	std::queue<AAMPEventPtr> mAdEventsQ;   // A Queue of Ad events
	std::mutex mAdEventQMtx;            // Add events' queue protector
	bool mInitSuccess;	//TODO: Need to replace with player state
	StreamOutputFormat mVideoFormat;
	StreamOutputFormat mAudioFormat;
	pthread_cond_t mDownloadsDisabled;
	bool mDownloadsEnabled;
	StreamSink* mStreamSink;

	ProfileEventAAMP profiler;
	bool licenceFromManifest;
	AudioType previousAudioType; /* Used to maintain previous audio type */

	CURL *curl[eCURLINSTANCE_MAX];
	CURLSH* mCurlShared;

	// To store Set Cookie: headers and X-Reason headers in HTTP Response
	httpRespHeaderData httpRespHeaders[eCURLINSTANCE_MAX];
	//std::string cookieHeaders[MAX_CURL_INSTANCE_COUNT]; //To store Set-Cookie: headers in HTTP response
	std::string  mManifestUrl;
	std::string mTunedManifestUrl;

	int mReportProgressInterval;					// To store the refresh interval in millisec
	int mPreCacheDnldTimeWindow;		// Stores PreCaching timewindow
	int mInitFragmentRetryCount;		// max attempts for init frag curl timeout failures
	bool mUseAvgBandwidthForABR;
	bool mbDownloadsBlocked;
	bool streamerIsActive;
	bool mTSBEnabled;
	bool mIscDVR;
	double mLiveOffset;
	long mNetworkTimeoutMs;
	long mManifestTimeoutMs;
	long mPlaylistTimeoutMs;
	bool mParallelFetchPlaylist;
	bool mParallelFetchPlaylistRefresh;
	bool mWesterosSinkEnabled;
	bool mEnableRectPropertyEnabled;
	bool mAsyncTuneEnabled;
	bool mBulkTimedMetadata;
	bool mDisableEC3;
	bool mDisableATMOS;
	bool mForceEC3;
	bool mUseRetuneForUnpairedDiscontinuity;
	bool mUseRetuneForGSTInternalError;
	long long prevPositionMiliseconds;
	MediaFormat mMediaFormat;
	bool mNewLiveOffsetflag;	
	pthread_t fragmentCollectorThreadID;
	double seek_pos_seconds; // indicates the playback position at which most recent playback activity began
	int rate; // most recent (non-zero) play rate for non-paused content
	bool pipeline_paused; // true if pipeline is paused
	char language[MAX_LANGUAGE_TAG_LENGTH];  // current language set
	bool noExplicitUserLanguageSelection; //true until not updated apart constructor
	bool languageSetByUser; //initially 'false', set 'true' once language[] is set by user (also with the same as init value)
	char mLanguageList[MAX_LANGUAGE_COUNT][MAX_LANGUAGE_TAG_LENGTH]; // list of languages in stream
	int mCurrentLanguageIndex; // Index of current selected lang in mLanguageList, this is used for VideoStat event data collection
	int  mMaxLanguageCount;
	std::string preferredLanguagesString; // unparsed string with preferred languages in format "lang1,lang2,.."
	std::vector<std::string> preferredLanguagesList; // list of preferred languages from most-preferred to the least
	VideoZoomMode zoom_mode;
	bool video_muted;
	bool subtitles_muted;
	int audio_volume;
	std::vector<std::string> subscribedTags;
	std::vector<TimedMetadata> timedMetadata;
	std::vector<TimedMetadata> reportMetadata;
	bool mIsIframeTrackPresent;				/**< flag to check iframe track availability*/

	/* START: Added As Part of DELIA-28363 and DELIA-28247 */
	bool IsTuneTypeNew; /* Flag for the eTUNETYPE_NEW_NORMAL */
	/* END: Added As Part of DELIA-28363 and DELIA-28247 */
	bool mLogTimetoTopProfile; /* Flag for logging time to top profile ,only one time after tune .*/
	pthread_cond_t waitforplaystart;    /**< Signaled after playback starts */
	pthread_mutex_t mMutexPlaystart;	/**< Mutex associated with playstart */
	long long trickStartUTCMS;
	long long playStartUTCMS;
	double durationSeconds;
	double culledSeconds;
	float maxRefreshPlaylistIntervalSecs;
	long long initialTuneTimeMs;
	EventListener* mEventListener;
	double mReportProgressPosn;
	long long mReportProgressTime;
	long long mAdPrevProgressTime;
	uint32_t mAdCurOffset;		//Start position in percentage
	uint32_t mAdDuration;
	std::string mAdProgressId;
	bool discardEnteringLiveEvt;
	bool mIsRetuneInProgress;
	pthread_cond_t mCondDiscontinuity;
	guint mDiscontinuityTuneOperationId;
	bool mIsVSS;       /**< Indicates if stream is VSS, updated during Tune*/
	long curlDLTimeout[eCURLINSTANCE_MAX]; /**< To store donwload timeout of each curl instance*/
	char mSubLanguage[MAX_LANGUAGE_TAG_LENGTH];   // current subtitle language set
	bool mPlayerPreBuffered;     // Player changed from BG to FG
	TunedEventConfig  mTuneEventConfigVod;
	TunedEventConfig mTuneEventConfigLive;
	int mPlayerId;
	int mRampDownLimit;
	int mSegInjectFailCount;	/**< Sets retry count for segment injection failure */
	int mDrmDecryptFailCount;	/**< Sets retry count for DRM decryption failure */
#ifdef AAMP_HLS_DRM
	std::vector <attrNameData> aesCtrAttrDataList; /**< Queue to hold the values of DRM data parsed from manifest */
	pthread_mutex_t drmParserMutex; /**< Mutex to lock DRM parsing logic */
	bool fragmentCdmEncrypted; /**< Indicates CDM protection added in fragments **/
#endif
	pthread_t mPreCachePlaylistThreadId;
	bool mPreCachePlaylistThreadFlag;
	bool mABRBufferCheckEnabled;
	bool mNewAdBreakerEnabled;
	bool mbPlayEnabled;	//Send buffer to pipeline or just cache them.
#if defined(AAMP_MPD_DRM) || defined(AAMP_HLS_DRM) || defined(USE_OPENCDM)
	pthread_t createDRMSessionThreadID; /**< thread ID for DRM session creation **/
	bool drmSessionThreadStarted; /**< flag to indicate the thread is running on not **/
	AampDRMSessionManager *mDRMSessionManager;
#endif
	long mPlaylistFetchFailError;	/**< To store HTTP error code when playlist download fails */
	bool mAudioDecoderStreamSync; /**< BCOM-4203: Flag to set or clear 'stream_sync_mode' property
	                                in gst brcmaudiodecoder, default: True */
	std::string mSessionToken; /**< Field to set session token for player */
	bool midFragmentSeekCache;    /**< RDK-26957: To find if cache is updated when seeked to mid fragment boundary*/

	/**
	 * @brief Curl initialization function
	 *
	 * @param[in] startIdx - Start index of the curl instance
	 * @param[in] instanceCount - Instance count
	 * @param[in] proxy - proxy to be applied for curl connection	 
	 * @return void
	 */
	void CurlInit(AampCurlInstance startIdx, unsigned int instanceCount=1,const char *proxy=NULL);

	/**
	 *   @brief Sets Recorded URL from Manifest received form XRE.
	 *   @param[in] isrecordedUrl - flag to check for recordedurl in Manifest
	 */
	void SetTunedManifestUrl(bool isrecordedUrl = false);

	/**
	 *   @brief Gets Recorded URL from Manifest received form XRE.
	 *   @param[out] manifestUrl - for VOD and recordedUrl for FOG enabled
	 */
	const char *GetTunedManifestUrl();

	/**
	 * @brief Set curl timeout
	 *
	 * @param[in] timeout - Timeout value
	 * @param[in] instance - Curl instance
	 * @return void
	 */
	void SetCurlTimeout(long timeout, AampCurlInstance instance);

	/**
	 * @brief Set manifest curl timeout
	 *
	 * @param[in] timeout - Timeout value in ms
	 * @return void
	 */
	void SetManifestCurlTimeout(long timeout);

	/**
	 * @brief Storing audio language list
	 *
	 * @param[in] langlist - Vector of languages
	 * @return void
	 */
	void StoreLanguageList(const std::set<std::string> &langlist);

	/**
	 * @brief Checking whether audio language supported
	 *
	 * @param[in] checkLanguage - Language to be checked
	 * @return True or False
	 */
	bool IsAudioLanguageSupported (const char *checkLanguage);

	/**
	 * @brief Terminate curl contexts
	 *
	 * @param[in] startIdx - First index
	 * @param[in] instanceCount - Instance count
	 * @return void
	 */
	void CurlTerm(AampCurlInstance startIdx, unsigned int instanceCount=1);

	/**
	 * @brief GetPlaylistCurlInstance - Get Curl Instance for playlist download
	 *
	 * @param[in] MediaType  - type of playlist
	 * @param[in] flag 		 - Init or Refresh download
	 * @return AampCurlInstance - curl instance for download
	 */
	AampCurlInstance GetPlaylistCurlInstance(MediaType type, bool IsInitDnld=true);


	/**
	 * @brief Download a file from the server
	 *
	 * @param[in] remoteUrl - File URL
	 * @param[out] buffer - Pointer to the output buffer
	 * @param[out] effectiveUrl - Final URL after HTTP redirection
	 * @param[out] http_error - HTTP error code
	 * @param[in] range - Byte range
	 * @param[in] curlInstance - Curl instance to be used
	 * @param[in] resetBuffer - Flag to reset the out buffer
	 * @param[in] fileType - File type
	 * @return void
	 */
	bool GetFile(std::string remoteUrl, struct GrowableBuffer *buffer, std::string& effectiveUrl, long *http_error = NULL, double *downloadTime = NULL, const char *range = NULL,unsigned int curlInstance = 0, bool resetBuffer = true,MediaType fileType = eMEDIATYPE_DEFAULT, long *bitrate = NULL,  int * fogError = NULL, double fragmentDurationSec = 0);

	/**
	 * @brief Download VideoEnd Session statistics from fog
	 *
	 * @param[out] buffer - Pointer to the output buffer
	 * @returrn string tsbSessionEnd data from fog
	 */
	char* GetOnVideoEndSessionStatData();

	/**
	 * @brief Perform custom get curl request
	 *
	 * @param[in] remoteUrl - File URL
	 * @param[out] buffer - Pointer to the output buffer
	 * @param[out] http_error - HTTP error code
	 * @return bool status
	 */
	bool ProcessCustomGetCurlRequest(std::string& remoteUrl, struct GrowableBuffer* buffer, long *http_error);

	/**
	 * @brief get Media Type in string
	 * @param[in] fileType - Type of Media
	 * @param[out] pointer to Media Type string
	 */
	const char* MediaTypeString(MediaType fileType);

	/**
	 * @brief Download fragment
	 *
	 * @param[in] bucketType - Bucket type of the profiler
	 * @param[in] fragmentUrl - Fragment URL
	 * @param[out] buffer - Pointer to the output buffer
	 * @param[out] len - Content length
	 * @param[in] curlInstance - Curl instance to be used
	 * @param[in] range - Byte range
	 * @param[in] fileType - File type
	 * @param[out] fogError - Error from FOG
	 * @return void
	 */
	char *LoadFragment( ProfilerBucketType bucketType, std::string fragmentUrl, std::string& effectiveUrl, size_t *len, unsigned int curlInstance = 0, const char *range = NULL,long * http_code = NULL, double *downloadTime = NULL, MediaType fileType = eMEDIATYPE_MANIFEST,int * fogError = NULL);

	/**
	 * @brief Download fragment
	 *
	 * @param[in] bucketType - Bucket type of the profiler
	 * @param[in] fragmentUrl - Fragment URL
	 * @param[out] buffer - Pointer to the output buffer
	 * @param[in] curlInstance - Curl instance to be used
	 * @param[in] range - Byte range
	 * @param[in] fileType - File type
	 * @param[out] http_code - HTTP error code
	 * @param[out] fogError - Error from FOG
	 * @return void
	 */
	bool LoadFragment( ProfilerBucketType bucketType, std::string fragmentUrl, std::string& effectiveUrl, struct GrowableBuffer *buffer, unsigned int curlInstance = 0, const char *range = NULL, MediaType fileType = eMEDIATYPE_MANIFEST, long * http_code = NULL, double * downloadTime = NULL, long *bitrate = NULL, int * fogError = NULL, double fragmentDurationSec = 0);

	/**
	 * @brief Push fragment to the gstreamer
	 *
	 * @param[in] mediaType - Media type
	 * @param[in] buffer - Pointer to the buffer
	 * @param[in] fragmentTime - Fragment start time
	 * @param[in] fragmentDuration - Fragment duration
	 * @return void
	 */
	void PushFragment(MediaType mediaType, char *ptr, size_t len, double fragmentTime, double fragmentDuration);

	/**
	 * @brief Push fragment to the gstreamer
	 *
	 * @param[in] mediaType - Media type
	 * @param[in] buffer - Pointer to the growable buffer
	 * @param[in] fragmentTime - Fragment start time
	 * @param[in] fragmentDuration - Fragment duration
	 * @return void
	 */
	void PushFragment(MediaType mediaType, GrowableBuffer* buffer, double fragmentTime, double fragmentDuration);

	/**
	 * @brief End of stream reached
	 *
	 * @param[in] mediaType - Media type
	 * @return void
	 */
	void EndOfStreamReached(MediaType mediaType);

	/**
	 * @brief Clip ended
	 *
	 * @param[in] mediaType - Media type
	 * @return void
	 */
	void EndTimeReached(MediaType mediaType);

	/**
	 * @brief Insert ad content
	 *
	 * @param[in] url - Ad url
	 * @param[in] positionSeconds - Ad start position in seconds
	 * @return void
	 */
	void InsertAd(const char *url, double positionSeconds);

	/**
	 * @brief Register event lister
	 *
	 * @param[in] eventType - Event type
	 * @param[in] eventListener - Event handler
	 * @return void
	 */
	void AddEventListener(AAMPEventType eventType, EventListener* eventListener);

	/**
	 * @brief Deregister event lister
	 *
	 * @param[in] eventType - Event type
	 * @param[in] eventListener - Event handler
	 * @return void
	 */
	void RemoveEventListener(AAMPEventType eventType, EventListener* eventListener);

	/**
	 * @brief Send events synchronously
	 *
	 * @param[in] e - Event object
	 * @return void
	 */
	void SendEventAsync(AAMPEventPtr e);

	/**
	 * @brief Handles errors and sends events to application if required.
	 * For download failures, use SendDownloadErrorEvent instead.
	 *
	 * @param[in] tuneFailure - Reason of error
	 * @param[in] description - Optional description of error
	 * @return void
	 */
	void SendErrorEvent(AAMPTuneFailure tuneFailure, const char *description = NULL, bool isRetryEnabled = true);

	

	void SendDRMMetaData(DrmMetaDataEventPtr e);

	/**
	 * @brief Handles DRM errors and sends events to application if required.
	 * @param[in] event aamp event struck which holds the error details and error code(http, curl or secclient).
	 * @param[in] isRetryEnabled drm retry enabled
	 */
	void SendDrmErrorEvent(DrmMetaDataEventPtr event, bool isRetryEnabled);

	/**
	 * @brief Handles download errors and sends events to application if required.
	 *
	 * @param[in] tuneFailure - Reason of error
	 * @param[in] error_code - HTTP error code/ CURLcode
	 * @return void
	 */
	void SendDownloadErrorEvent(AAMPTuneFailure tuneFailure,long error_code);

	/**
	 * @brief Sends Anomaly Error/warning messages
	 *
	 * @param[in] type - severity of message
	 * @param[in] format - format string
	 * args [in]  - multiple arguments based on format
	 * @return void
	 */
	void SendAnomalyEvent(AAMPAnomalyMessageType type, const char* format, ...);

	void SendBufferChangeEvent(bool bufferingStopped=false);

	/* Buffer Under flow status flag, under flow Start(buffering stopped) is true and under flow end is false*/
	bool mBufUnderFlowStatus;
	bool GetBufUnderFlowStatus() { return mBufUnderFlowStatus; }
	void SetBufUnderFlowStatus(bool statusFlag) { mBufUnderFlowStatus = statusFlag; }
	void ResetBufUnderFlowStatus() { mBufUnderFlowStatus = false;}

	/**
	 * @brief Send events synchronously
	 *
	 * @param[in] e - Event object
	 * @return void
	 */
	void SendEventSync(AAMPEventPtr e);

	/**
	 * @brief Notify speed change
	 *
	 * @param[in] rate - New speed
	 * @param[in] changeState - true if state change to be done, false otherwise (default = true)
	 * @return void
	 */
	void NotifySpeedChanged(int rate, bool changeState = true);

	/**
	 * @brief Notify bit rate change
	 *
	 * @param[in] bitrate - New bitrate
	 * @param[in] reason - Bitrate change reason
	 * @param[in] width - Video width
	 * @param[in] height - Video height
 	 * @param[in] framerate - FRAME-RATE from manifest
	 * @param[in] GetBWIndex - Flag to get the bandwidth index
	 * @return void
	 */
	void NotifyBitRateChangeEvent(int bitrate, BitrateChangeReason reason, int width, int height, double framerate, double position, bool GetBWIndex = false);

	/**
	 * @brief Notify when end of stream reached
	 *
	 * @return void
	 */
	void NotifyEOSReached();

	/**
	 * @brief Notify when entering live point
	 *
	 * @return void
	 */
	void NotifyOnEnteringLive();

	/**
	 * @brief Get persisted profile index
	 *
	 * @return Profile index
	 */
	int  GetPersistedProfileIndex() {return mPersistedProfileIndex;}

	/**
	 * @brief Set persisted profile index
	 *
	 * @param[in] profile - Profile index
	 * @return void
	 */
	void SetPersistedProfileIndex(int profile){mPersistedProfileIndex = profile;}

	/**
	 * @brief Set persisted bandwidth
	 *
	 * @param[in] bandwidth - Bandwidth in bps
	 * @return void
	 */
	void SetPersistedBandwidth(long bandwidth) {mAvailableBandwidth = bandwidth;}

	/**
	 * @brief Get persisted bandwidth
	 *
	 * @return Bandwitdh
	 */
	long GetPersistedBandwidth(){return mAvailableBandwidth;}

	/**
	 * @brief Update playlist duration
	 *
	 * @param[in] seconds - Duration in seconds
	 * @return void
	 */
	void UpdateDuration(double seconds);

	/**
	 * @brief Update playlist culling
	 *
	 * @param[in] culledSeconds - Seconds to be culled
	 * @return void
	 */
	void UpdateCullingState(double culledSeconds);

	/**
	 *   @brief  Update playlist refresh inrerval
	 *
	 *   @param[in]  maxIntervalSecs - Interval in seconds
	 *   @return void
	 */
	void UpdateRefreshPlaylistInterval(float maxIntervalSecs);

	/**
	 *   @brief Report progress event
	 *
	 *   @return void
	 */
	void ReportProgress(void);

	/**
	 *   @brief Report Ad progress event
	 *
	 *   @return void
	 */
	void ReportAdProgress(void);

	/**
	 *   @brief Get asset duration in milliseconds
	 *
	 *   @return Duration in ms.
	 */
	long long GetDurationMs(void);

	/**
	 *   @brief Get playback position in milliseconds
	 *
	 *   @return Position in ms.
	 */
	long long GetPositionMs(void);

	/**
	 *   @brief Get playback position in milliseconds
	 *
	 *   @return Position in ms.
	 */
	long long GetPositionMilliseconds(void);

	/**
	 *   @brief  API to send audio/video stream into the sink.
	 *
	 *   @param[in]  mediaType - Type of the media.
	 *   @param[in]  ptr - Pointer to the buffer.
	 *   @param[in]  len - Buffer length.
	 *   @param[in]  fpts - Presentation Time Stamp.
	 *   @param[in]  fdts - Decode Time Stamp
	 *   @param[in]  duration - Buffer duration.
	 *   @return void
	 */
	void SendStream(MediaType mediaType, const void *ptr, size_t len, double fpts, double fdts, double fDuration);

	/**
	 *   @brief  API to send audio/video stream into the sink.
	 *
	 *   @param[in]  mediaType - Type of the media.
	 *   @param[in]  buffer - Pointer to the GrowableBuffer.
	 *   @param[in]  fpts - Presentation Time Stamp.
	 *   @param[in]  fdts - Decode Time Stamp
	 *   @param[in]  fDuration - Buffer duration.
	 *   @return void
	 */
	void SendStream(MediaType mediaType, GrowableBuffer* buffer, double fpts, double fdts, double fDuration);

	/**
	 * @brief Setting the stream sink
	 *
	 * @param[in] streamSink - Pointer to the stream sink
	 * @return void
	 */
	void SetStreamSink(StreamSink* streamSink);

	/**
	 * @brief Checking if the stream is live or not
	 *
	 * @return True or False
	 */
	bool IsLive(void);

	/**
	 * @brief Stop playback
	 *
	 * @return void
	 */
	void Stop(void);

	/**
	 * @brief Checking whether TSB enabled or not
	 *
	 * @return True or False
	 */
	bool IsTSBSupported() { return mTSBEnabled;}

	/**
	 * @brief Checking whether CDVR in progress
	 *
	 * @return True or False
	 */
	bool IsInProgressCDVR() {return (IsLive() && IsCDVRContent());}
	/**
	* @brief Checking whether CDVR Stream or not
	*
	* @return True or False
	*/
	bool IsCDVRContent() { return (mContentType==ContentType_CDVR || mIscDVR);}
	/**
	 * @brief Report timed metadata
	 *
	 * @param[in] timeMS - Time in milliseconds
	 * @param[in] szName - Metadata name
	 * @param[in] szContent - Metadata content
	 * @param[in] nb - ContentSize
	 * @param[in] bSyncCall - Sync /Async Event reporting
	 * @param[in] id - Identifier of the TimedMetadata
	 * @param[in] durationMS - Duration in milliseconds
	 * @return void
	 */
	void ReportTimedMetadata(long long timeMS, const char* szName, const char* szContent, int nb, bool bSyncCall=false,const char* id = "", double durationMS = -1);

	/**
	 * @brief Save timed metadata for later bulk reporting
	 *
	 * @param[in] timeMS - Time in milliseconds
	 * @param[in] szName - Metadata name
	 * @param[in] szContent - Metadata content
	 * @param[in] nb - ContentSize
	 * @param[in] id - Identifier of the TimedMetadata
	 * @param[in] durationMS - Duration in milliseconds
	 * @return void
	 */
	void SaveTimedMetadata(long long timeMS, const char* szName, const char* szContent, int nb, const char* id = "", double durationMS = -1);

	/**
	 * @brief Report bulk timedMetadata
	 *
	 * @return void
	 */
	void ReportBulkTimedMetadata();

	/**
	 * @brief sleep only if aamp downloads are enabled.
	 * interrupted on aamp_DisableDownloads() call
	 *
	 * @param[in] timeInMs
	 * @return void
	 */
	void InterruptableMsSleep(int timeInMs);

#ifdef AAMP_HARVEST_SUPPORT_ENABLED
	/**
	 * @brief Collect decrypted fragments
	 *
	 * @param[in] modifyCount - Collect only the configured number of fragments
	 * @return void
	 */
	bool HarvestFragments(bool modifyCount = true);
#endif

	/**
	 * @brief Get download disable status
	 *
	 * @return void
	 */
	bool DownloadsAreEnabled(void);

	/**
	 * @brief Stop downloads of all tracks.
	 * Used by aamp internally to manage states
	 *
	 * @return void
	 */
	void StopDownloads();

	/**
 	 * @brief Resume downloads of all tracks.
	 * Used by aamp internally to manage states
	 *
	 * @return void
	 */
	void ResumeDownloads();

	/**
	 * @brief Stop downloads for a track.
	 * Called from StreamSink to control flow
	 *
	 * @param[in] Media type
	 * @return void
	 */
	void StopTrackDownloads(MediaType);

	/**
 	 * @brief Resume downloads for a track.
	 * Called from StreamSink to control flow
	 *
	 * @param[in] Media type
	 * @return void
	 */
	void ResumeTrackDownloads(MediaType);

	/**
	 *   @brief Block the injector thread until the gstreanmer needs buffer.
	 *
	 *   @param[in] cb - Callback helping to perform additional tasks, if gst doesn't need extra data
	 *   @param[in] periodMs - Delay between callbacks
	 *   @param[in] track - Track id
	 *   @return void
	 */
	void BlockUntilGstreamerWantsData(void(*cb)(void), int periodMs, int track);

	/**
	 *   @brief Notify the tune complete event
	 *
	 *   @return void
	 */
	void LogTuneComplete(void);

	/**
	 *   @brief Profile first frame displayed
	 *
	 *   @return void
	 */
	void LogFirstFrame(void);

	/**
	 *   @brief Profile Player changed from background to foreground i.e prebuffred
	 *
	 *   @return void
	 */
       void LogPlayerPreBuffered(void);

	/**
	 *   @brief Drm license acquisition end profiling
	 *
	 *   @return void
	 */
	void LogDrmInitComplete(void);

	/**
	 *   @brief Drm decrypt begin profiling
	 *
	 *   @param[in] bucketType - Bucket Id
	 *   @return void
	 */
	void LogDrmDecryptBegin( ProfilerBucketType bucketType );

	/**
	 *   @brief Drm decrypt end profiling
	 *
	 *   @param[in] bucketType - Bucket Id
	 *   @return void
	 */
	void LogDrmDecryptEnd( ProfilerBucketType bucketType );
	
	/**
	 *   @brief Get manifest URL
	 *
	 *   @return Manifest URL
	 */
	std::string& GetManifestUrl(void)
	{
		return mManifestUrl;
	}

	/**
	 *   @brief Get DRM init data obtained from manifest URL (if present)
	 *
	 *   @return DRM init data
	 */
	std::string& GetDrmInitData(void)
	{
		return mDrmInitData;
	}

	/**
	 *   @brief Set manifest URL
	 *
	 *   @param[in] url - Manifest URL
	 *   @return void
	 */
	void SetManifestUrl(const char *url)
	{
		mManifestUrl.assign(url);
	}

	/**
	 *   @brief First frame received notification
	 *
	 *   @return void
	 */
	void NotifyFirstFrameReceived(void);

	/**
	 *   @brief GStreamer operation start
	 *
	 *   @return void
	 */
	void SyncBegin(void);

	/**
	 * @brief GStreamer operation end
	 *
	 * @return void
	 */
	void SyncEnd(void);

	/**
	 * @brief Get seek position
	 *
	 * @return Position in seconds
	 */
	double GetSeekBase(void);

	/**
	 * @brief Reset bandwidth value
	 * Artificially resetting the bandwidth. Low for quicker tune times
	 *
	 * @param[in] bitsPerSecond - bps
	 * @param[in] trickPlay		- Is trickplay mode
	 * @param[in] profile		- Profile id.
	 * @return void
	 */
	void ResetCurrentlyAvailableBandwidth(long bitsPerSecond,bool trickPlay,int profile=0);

	/**
	 * @brief Get the current network bandwidth
	 *
	 * @return Available bandwidth in bps
	 */
	long GetCurrentlyAvailableBandwidth(void);

	/**
	 * @brief Abort ongoing downloads and returns error on future downloads
	 * Called while stopping fragment collector thread
	 *
	 * @return void
	 */
	void DisableDownloads(void);

	/**
	 * @brief Enable downloads after aamp_DisableDownloads.
	 * Called after stopping fragment collector thread
	 *
	 * @return void
	 */
	void EnableDownloads(void);

	/**
	 *   @brief Register event listener
	 *
	 *   @param[in] eventListener - Handle to event listener
	 *   @return void
	 */
	void RegisterEvents(EventListener* eventListener)
	{
		mEventListener = eventListener;
	}

	/**
	 *   @brief Schedule retune
	 *
	 *   @param[in] errorType - Current error type
	 *   @param[in] trackType - Video/Audio
	 *   @return void
	 */
	void ScheduleRetune(PlaybackErrorType errorType, MediaType trackType);

	/**
	 * @brief PrivateInstanceAAMP Constructor
	 */
	PrivateInstanceAAMP();

	/**
	 * @brief PrivateInstanceAAMP Destructor
	 */
	~PrivateInstanceAAMP();

	PrivateInstanceAAMP(const PrivateInstanceAAMP&) = delete;

	PrivateInstanceAAMP& operator=(const PrivateInstanceAAMP&) = delete;

	/**
	 *   @brief Set video rectangle
	 *
	 *   @param[in] x - Left
	 *   @param[in] y - Top
	 *   @param[in] w - Width
	 *   @param[in] h - Height
	 *   @return void
	 */
	void SetVideoRectangle(int x, int y, int w, int h);

	/**
	 *   @brief Signal discontinuity of track.
	 *   Called from StreamAbstractionAAMP to signal discontinuity
	 *
	 *   @param[in] track - Media type
	 *   @param[in] setDiscontinuityFlag if true then no need to call mStreamSink->Discontinuity(), set only the discontinuity processing flag.
	 *   @return true if discontinuity is handled.
	 */
	bool Discontinuity(MediaType track, bool setDiscontinuityFlag = false);

	/**
	 *   @brief Set video zoom mode
	 *
	 *   @param[in] zoom - Video zoom mode
	 *   @return void
	 */
	void SetVideoZoom(VideoZoomMode zoom);

	/**
	 *   @brief Set video mute state
	 *
	 *   @param[in] muted - muted or unmuted
	 *   @return void
	 */
	void SetVideoMute(bool muted);

	/**
	 *   @brief Set audio volume
	 *
	 *   @param[in] volume - Volume level
	 *   @return void
	 */
	void SetAudioVolume(int volume);

	/**
	 *   @brief Set player state
	 *
	 *   @param[in] state - New state
	 *   @return void
	 */
	void SetState(PrivAAMPState state);

	/**
	 *   @brief Get player state
	 *
	 *   @param[out] state - Player state
	 *   @return void
	 */
	void GetState(PrivAAMPState &state);

	/**
	 *   @brief Add idle task to the gstreamer
	 *
	 *   @param[in] task - Task
	 *   @param[in] arg - Arguments
	 *   @return void
	 */
	static void AddIdleTask(IdleTask task, void* arg);
	/**
	*   @brief Add high priority idle task to the gstreamer
	*
	*   @param[in] task - Task
	*   @param[in] arg - Arguments
	*
	*   @return void
	*/
	static gint AddHighIdleTask(IdleTask task, void* arg,DestroyTask dtask=NULL);

	/**
	 *   @brief Check sink cache empty
	 *
	 *   @param[in] mediaType - Audio/Video
	 *   @return true: empty, false: not empty
	 */
	bool IsSinkCacheEmpty(MediaType mediaType);

	/**
	 *   @brief Notify fragment caching complete
	 *
	 *   @return void
	 */
	void NotifyFragmentCachingComplete();

	/**
	 *   @brief Send tuned event
	 *
	 *   @param[in] isSynchronous - send event synchronously or not
	 *   @return success or failure
	 */
	bool SendTunedEvent(bool isSynchronous = true);

	/**
	 *   @brief Send VideoEndEvent
	 *
	 *   @return success or failure
	 */
	bool SendVideoEndEvent();

	/**
	 *   @brief Check if fragment caching is required
	 *
	 *   @return true if required or ongoing, false if not needed
	 */
	bool IsFragmentCachingRequired();

	/**
	 *   @brief Get player video size
	 *
	 *   @param[out] w - Width
	 *   @param[out] h - Height
	 *   @return void
	 */
	void GetPlayerVideoSize(int &w, int &h);

	/**
	 *   @brief Set callback as event pending
	 *
	 *   @param[in] id - Callback id.
	 *   @return void
	 */
	void SetCallbackAsPending(guint id);

	/**
	 *   @brief Set callback as event dispatched
	 *
	 *   @param[in] id - Callback id.
	 *   @return void
	 */
	void SetCallbackAsDispatched(guint id);


	/**
	 *   @brief Add custom HTTP header
	 *
	 *   @param[in] headerName  - Header name
	 *   @param[in] headerValue - Header value
	 *   @param[in] isLicenseHeader - true if header is for a license request
	 *   @return void
	 */
	void AddCustomHTTPHeader(std::string headerName, std::vector<std::string> headerValue, bool isLicenseHeader);

	/**
	 *   @brief Set license server URL
	 *
	 *   @param[in] url - server URL
	 *   @param[in] drmType - DRM type (PR/WV) for which the URL has to be used, global by default
	 *   @return void
	 */
	void SetLicenseServerURL(const char* url, DRMSystems drmType = eDRM_MAX_DRMSystems);

	/**
	 *   @brief Set Preferred DRM.
	 *
	 *   @param[in] drmType - Preferred DRM type
	 *   @return void
	 */
	void SetPreferredDRM(DRMSystems drmType);

	/**
	 *   @brief Get Preferred DRM.
	 *
	 *   @return Preferred DRM type
	 */
	DRMSystems GetPreferredDRM();

	/**
	 *   @brief Set Stereo Only Playback.
	 *   @param[in] bValue - disable EC3/ATMOS if the value is true
	 *
	 *   @return void
	 */
	void SetStereoOnlyPlayback(bool bValue);

	/**
	 *   @brief Set Bulk TimedMetadata Reporting flag
	 *   @param[in] bValue - if true Bulk event reporting enabled
	 *
	 *   @return void
	 */
	void SetBulkTimedMetaReport(bool bValue);

	/**
	 *	 @brief Set unpaired discontinuity retune flag
	 *	 @param[in] bValue - true if unpaired discontinuity retune set
	 *
	 *	 @return void
	 */
	void SetRetuneForUnpairedDiscontinuity(bool bValue);

	/**
	 *	 @brief Set retune configuration for gstpipeline internal data stream error.
	 *	 @param[in] bValue - true if gst internal error retune set
	 *
	 *	 @return void
	 */
	void SetRetuneForGSTInternalError(bool bValue);

	/**
	 *   @brief Notification from the stream abstraction that a new SCTE35 event is found.
	 *
	 *   @param[in] Adbreak's unique identifier.
	 *   @param[in] Break start time in milli seconds.
	 *   @param[in] Break duration in milli seconds
	 *   @param[in] SCTE35 binary object.
	 */
	void FoundSCTE35(const std::string &adBreakId, uint64_t startMS, uint32_t breakdur, std::string &scte35);

	/**
	 *   @brief Setting the alternate contents' (Ads/blackouts) URL
	 *
	 *   @param[in] Adbreak's unique identifier.
	 *   @param[in] Individual Ad's id
	 *   @param[in] Ad URL
	 */
	void SetAlternateContents(const std::string &adBreakId, const std::string &adId, const std::string &url);

	/**
	 *   @brief Send status of Ad manifest downloading & parsing
	 *
	 *   @param[in] Ad's unique identifier.
	 *   @param[in] Manifest status (success/Failure)
	 *   @param[in] Ad playback start time in milliseconds
	 *   @param[in] Ad's duration in milliseconds
	 */
	void SendAdResolvedEvent(const std::string &adId, bool status, uint64_t startMS=0, uint64_t durationMs=0);

	/**
	 * @brief Send status of Events corresponding to Ad reservation
	 *
	 * @param[in] tuneFailure - Reason of error
	 * @param[in] error_code - HTTP error code/ CURLcode
	 *
	 * @return void
	 */
	void SendAdReservationEvent(AAMPEventType type, const std::string &adBreakId, uint64_t position, bool immediate=false);

	/**
	 * @brief Send status of Events corresponding to Ad placement
	 *
	 * @param[in] tuneFailure - Reason of error
	 * @param[in] error_code - HTTP error code/ CURLcode
	 *
	 * @return void
	 */
	void SendAdPlacementEvent(AAMPEventType type, const std::string &adId, uint32_t position, uint32_t adOffset, uint32_t adDuration, bool immediate=false, long error_code=0);

	/**
	 *   @brief Set anonymous request true or false
	 *
	 *   @param[in] isAnonymous - New status
	 *   @return void
	 */
	void SetAnonymousRequest(bool isAnonymous);

	/**
	 *   @brief Indicates average BW to be used for ABR Profiling.
	 *
	 *   @param  useAvgBW - Flag for true / false
	 */
	void SetAvgBWForABR(bool useAvgBW);
	/**
	*   @brief SetPreCacheTimeWindow Function to Set PreCache Time
	*
	*   @param  Time in minutes - Max PreCache Time 
	*/
	void SetPreCacheTimeWindow(int nTimeWindow);
	/**
	 *   @brief Set frames per second for VOD trickplay
	 *
	 *   @param[in] vodTrickplayFPS - FPS count
	 *   @return void
	 */
	void SetVODTrickplayFPS(int vodTrickplayFPS);

	/**
	 *   @brief Set frames per second for linear trickplay
	 *
	 *   @param[in] linearTrickplayFPS - FPS count
	 *   @return void
	 */
	void SetLinearTrickplayFPS(int linearTrickplayFPS);

	/**
	 *   @brief Sets live offset [Sec]
	 *
	 *   @param[in] SetLiveOffset - Live Offset
	 *   @return void
	 */

	void SetLiveOffset(int SetLiveOffset);

	/**
	 *   @brief Set stall error code
	 *
	 *   @param[in] errorCode - Stall error code
	 *   @return void
	 */
	void SetStallErrorCode(int errorCode);

	/**
	 *   @brief Set stall timeout
	 *
	 *   @param[in] timeoutMS - Timeout in milliseconds
	 *   @return void
	 */
	void SetStallTimeout(int timeoutMS);

	/**
	 *   @brief To set the Playback Position reporting interval.
	 *
	 *   @param  reportIntervalMS - playback reporting interval in milliseconds.
	 */
	void SetReportInterval(int reportIntervalMS);

	/**
	 *	 @brief To set the max retry attempts for init frag curl timeout failures
	 *
	 *	 @param  count - max attempt for timeout retry count
	 */
	void SetInitFragTimeoutRetryCount(int count);

	/**
	 *   @brief Send stalled error
	 *
	 *   @return void
	 */
	void SendStalledErrorEvent();

	/**
	 *   @brief Is discontinuity pending to process
	 *
	 *   @return void
	 */
	bool IsDiscontinuityProcessPending();

	/**
	 *   @brief Process pending discontinuity
	 *
	 *   @return true if pending discontinuity was processed successful, false if interrupted
	 */
	bool ProcessPendingDiscontinuity();

	/**
	 *   @brief Notify if first buffer processed by gstreamer
	 *
	 *   @return void
	 */
	void NotifyFirstBufferProcessed();

	/**
	 *   @brief Update audio language selection
	 *
	 *   @param[in] lang - Language
	 *   @param[in] checkBeforeOverwrite - flag to enable additional check before overwriting language
	 *   @return void
	 */
	void UpdateAudioLanguageSelection(const char *lang, bool checkBeforeOverwrite = false);

	/**
	 *   @brief Update subtitle language selection
	 *
	 *   @param[in] lang - Language
	 *   @return void
	 */
	void UpdateSubtitleLanguageSelection(const char *lang);

	/**
	 *   @brief Get stream type
	 *
	 *   @return Stream type
	 */
	int getStreamType();

	/**
	 *   @brief Get stream type as printable format
	 *
	 *   @return Stream type as string
	 */
	std::string getStreamTypeString();

	/**
	 *   @brief Get current drm
	 *
	 *   @return current drm helper
	 */
	std::shared_ptr<AampDrmHelper>  GetCurrentDRM();

	/**
	 *   @brief Set DRM type
	 *
	 *   @param[in] drm - New DRM type
	 *   @return void
	 */
	void setCurrentDrm(std::shared_ptr<AampDrmHelper> drm) { mCurrentDrm = drm; }

	/**
	 *   @brief Check if current  playback is from local TSB
	 *
	 *   @return true: yes, false: no
	 */
	bool IsLocalPlayback() { return mIsLocalPlayback; }

#ifdef USE_SECCLIENT
	/**
	 * @brief Extracts / Generates MoneyTrace string
	 * @param[out] customHeader - Generated moneytrace is stored
	 *
	 * @return void
	 */
	void GetMoneyTraceString(std::string &) const;
#endif /* USE_SECCLIENT */

	/**
	 *   @brief Notify the decryption completion of the fist fragment.
	 *
	 *   @return void
	 */
	void NotifyFirstFragmentDecrypted();

	/**
	 *   @brief  Get PTS of first sample.
	 *
	 *   @return PTS of first sample
	 */
	double GetFirstPTS();

	/**
	 *   @brief Check if Live Adjust is required for current content. ( For "vod/ivod/ip-dvr/cdvr/eas", Live Adjust is not required ).
	 *
	 *   @return False if the content is either vod/ivod/cdvr/ip-dvr/eas
	 */
	bool IsLiveAdjustRequired();

	/**
	 *   @brief  Generate media metadata event based on args passed.
	 *
	 *   @param[in] durationMs - duration of playlist in milliseconds
	 *   @param[in] langList - list of audio language available in asset
	 *   @param[in] bitrateList - list of video bitrates available in asset
	 *   @param[in] hasDrm - indicates if asset is encrypted/clear
	 *   @param[in] isIframeTrackPresent - indicates if iframe tracks are available in asset
	 */
	void SendMediaMetadataEvent(double durationMs, std::set<std::string>langList, std::vector<long> bitrateList, bool hasDrm, bool isIframePresent);

	/**
	 *   @brief  Generate supported speeds changed event based on arg passed.
	 *
	 *   @param[in] isIframeTrackPresent - indicates if iframe tracks are available in asset
	 */
	void SendSupportedSpeedsChangedEvent(bool isIframeTrackPresent);

	/**
	 *   @brief  Generate Blocked  event based on args passed.
	 *
	 *   @param[in] reason          - Blocked Reason
	 */
	void SendBlockedEvent(const std::string & reason);

	/**
	 *   @brief To set the initial bitrate value.
	 *
	 *   @param[in] initial bitrate to be selected
	 */
	void SetInitialBitrate(long bitrate);

	/**
	 *   @brief To set the initial bitrate value for 4K assets.
	 *
	 *   @param[in] initial bitrate to be selected for 4K assets
	 */
	void SetInitialBitrate4K(long bitrate4K);

	/**
	 *   @brief To set the network download timeout value.
	 *
	 *   @param[in] preferred timeout value
	 */
	void SetNetworkTimeout(double timeout);
	/**
	 *   @brief To set the network timeout as per priority
	 *
	 */
	void ConfigureNetworkTimeout();	
	/**
	 *   @brief To set the manifest timeout as per priority
	 *
	*/
	void ConfigureManifestTimeout();
	/**
	*   @brief To set the manifest timeout as per priority
	*
	*/
	void ConfigurePlaylistTimeout();

	/**
	*   @brief To set the parallel playlist fetch configuration
	*
	*/
	void ConfigureParallelFetch();
	/**
	*   @brief To set bulk timedMetadata reporting
	*
	*/
	void ConfigureBulkTimedMetadata();

	/**
	 *	 @brief To set unpaired discontinuity retune configuration
	 *
	 */
	void ConfigureRetuneForUnpairedDiscontinuity();

	/**
	 *	 @brief To set retune configuration for gstpipeline internal data stream error.
	 *
	 */
	void ConfigureRetuneForGSTInternalError();

	/**
	 *	 @brief Function to configure PreCachePlaylist
	 *
	 */
	void ConfigurePreCachePlaylist();

	/**
	 *	 @brief Function to set the max retry attempts for init frag curl timeout failures
	 *
	 */
	void ConfigureInitFragTimeoutRetryCount();

	/**
	 *	 @brief To set westeros sink configuration
	 *
	 */
	void ConfigureWesterosSink();

	/**
	 *   @brief To set the manifest download timeout value.
	 *
	 *   @param[in] preferred timeout value
	 */
	void SetManifestTimeout(double timeout);
	/**
	*   @brief To set the playlist download timeout value.
	*
	*   @param[in] preferred timeout value
	*/
	void SetPlaylistTimeout(double timeout);

	/**
	 *   @brief To set the download buffer size value
	 *
	 *   @param[in] preferred download buffer size
	 */
	void SetDownloadBufferSize(int bufferSize);

	/**
	 *   @brief  Check if tune completed or not.
	 *
	 *   @return true, if tune completed.
	 */
	bool IsTuneCompleted();

	/**
	 *   @brief Check if ABR enabled for this playback session.
	 *
	 *   @return true if ABR enabled.
	 */
	bool CheckABREnabled(void) { return mABREnabled; }
	/**
	 *   @brief Set a preferred bitrate for video.
	 *
	 *   @param[in] preferred bitrate.
	 */
	void SetVideoBitrate(long bitrate);

	/**
	 *   @brief Get preferred bitrate for video.
	 *
	 *   @return preferred bitrate.
	 */
	long GetVideoBitrate();

	/**
	 *   @brief To set the network proxy
	 *
	 *   @param[in] network proxy to use
	 */
	void SetNetworkProxy(const char * proxy);

	/**
	 *   @brief To get the network proxy
	 *
	 *   @return Network proxy URL, if exists.
	 */
	const char* GetNetworkProxy() const;

	/**
	 *   @brief To set the proxy for license request
	 *
	 *   @param[in] proxy to use for license request
	 */
	void SetLicenseReqProxy(const char * licenseProxy);

	/**
	 *   @brief To get the proxy for license request
	 *
	 *   @return proxy to use for license request
	 */
	char * GetLicenseReqProxy() { return mLicenseProxy; };

	/**
	 *   @brief Set is Live flag
	 *
	 *   @param[in] isLive - is Live flag
	 *   @return void
	 */
	void SetIsLive(bool isLive)  {mIsLive = isLive; }

	/**
	 *   @brief Signal trick mode discontinuity to stream sink
	 *
	 *   @return void
	 */
	void SignalTrickModeDiscontinuity();

	/**
	 *   @brief  pass service zone, extracted from locator &sz URI parameter
	 *   @return std::string
	 */
	std::string GetServiceZone() const{ return mServiceZone; }

	/**
	 *   @brief  pass virtual stream ID
	 *   @return std::string
	 */
	std::string GetVssVirtualStreamID() const{ return mVssVirtualStreamId; }

	/**
	 *   @brief  set virtual stream ID, extracted from manifest
	 */
	void SetVssVirtualStreamID(std::string streamID) { mVssVirtualStreamId = streamID;}

	/**
	 *   @brief IsNewTune Function to check if tune is New tune or retune
	 *
	 *   @return Bool True on new tune
	 */
	bool IsNewTune()  { return ((eTUNETYPE_NEW_NORMAL == mTuneType) || (eTUNETYPE_NEW_SEEK == mTuneType)); }

	/**
	 *   @brief Check if current stream is muxed
	 *
	 *   @return true if current stream is muxed
	 */
	bool IsMuxedStream();

	/**
	 *   @brief To set the curl stall timeout value
	 *
	 *   @param[in] curl stall timeout
	 */
	void SetDownloadStallTimeout(long stallTimeout);

	/**
	 *   @brief To set the curl download start timeout value
	 *
	 *   @param[in] curl download start timeout
	 */
	void SetDownloadStartTimeout(long startTimeout);

	/**
	 * @brief Stop injection for a track.
	 * Called from StopInjection
	 *
	 * @param[in] Media type
	 * @return void
	 */
	void StopTrackInjection(MediaType type);

	/**
	 * @brief Resume injection for a track.
	 * Called from StartInjection
	 *
	 * @param[in] Media type
	 * @return void
	 */
	void ResumeTrackInjection(MediaType type);

	/**
	 *   @brief Receives first video PTS of the current playback
	 *
	 *   @param[in]  pts - pts value
	 */
	void NotifyFirstVideoPTS(unsigned long long pts);

	/**
	 *   @brief To send webvtt cue as an event
	 *
	 *   @param[in]  cue - vtt cue object
	 */
	void SendVTTCueDataAsEvent(VTTCue* cue);

	/**
	 *   @brief To check if subtitles are enabled
	 *
	 *   @return bool - true if subtitles are enabled
	 */
	bool IsSubtitleEnabled(void);
	
	/**
	 *   @brief To check if JavaScript cue listeners are registered
	 *
	 *   @return bool - true if listeners are registered
	 */
	bool WebVTTCueListenersRegistered(void);

	/**   @brief updates download metrics to VideoStat object,
	 *
	 *   @param[in]  mediaType - MediaType ( Manifest/Audio/Video etc )
	 *   @param[in]  bitrate - bitrate ( bits per sec )
	 *   @param[in]  curlOrHTTPErrorCode - download curl or http error
	 *   @param[in]  strUrl :  URL in case of faulures
	 *   @return void
	 */
	void UpdateVideoEndMetrics(MediaType mediaType, long bitrate, int curlOrHTTPCode, std::string& strUrl, double curlDownloadTime);

	/**   @brief updates download metrics to VideoStat object
	 *
	 *   @param[in]  mediaType - MediaType ( Manifest/Audio/Video etc )
	 *   @param[in]  bitrate - bitrate ( bits per sec )
	 *   @param[in]  width - Frame width
	 *   @param[in]  Height - Frame Height
	 *   @return void
	 */
	void UpdateVideoEndProfileResolution(MediaType mediaType, long bitrate, int width, int height);

	/**
	 *   @brief updates time shift buffer status
	 *
	 *   @param[in]  btsbAvailable - true if TSB supported
	 *   @return void
	 */
	void UpdateVideoEndTsbStatus(bool btsbAvailable);

	/**
	*   @brief updates download metrics to VideoStat object, this is used for VideoFragment as it takes duration for calcuation purpose.
	*
	*  @param[in]  mediaType - MediaType ( Manifest/Audio/Video etc )
	*   @param[in]  bitrate - bitrate ( bits per sec )
	*   @param[in]  curlOrHTTPErrorCode - download curl or http error
	*   @param[in]  strUrl :  URL in case of faulures
	*   @param[in] keyChanged : if DRM key changed then it is set to true
	*   @param[in] isEncrypted : if fragment is encrypted then it is set to true
	*   @return void
	*/
	void UpdateVideoEndMetrics(MediaType mediaType, long bitrate, int curlOrHTTPCode, std::string& strUrl, double duration,double curlDownloadTime, bool keyChanged, bool isEncrypted);
    
	/**
	*   @brief updates download metrics to VideoStat object, this is used for VideoFragment as it takes duration for calcuation purpose.
	*
	*   @param[in]  mediaType - MediaType ( Manifest/Audio/Video etc )
	*   @param[in]  bitrate - bitrate ( bits per sec )
	*   @param[in]  curlOrHTTPErrorCode - download curl or http error
	*   @param[in]  strUrl :  URL in case of faulures
	*   @return void
	*/
	void UpdateVideoEndMetrics(MediaType mediaType, long bitrate, int curlOrHTTPCode, std::string& strUrl, double duration, double curlDownloadTime);


	/**
	 *   @brief updates abr metrics to VideoStat object,
	 *
	 *   @param[in]  AAMPAbrInfo - abr info
	 *   @return void
	 */
	void UpdateVideoEndMetrics(AAMPAbrInfo & info);


	/**
	 *   @brief To check if current asset is DASH or not
	 *
	 *   @return bool - true if its DASH asset
	 */
	bool IsDashAsset(void) { return (mMediaFormat==eMEDIAFORMAT_DASH); }

	/**
	 *   @brief Check if AAMP is in stalled state after it pushed EOS to
	 *   notify discontinuity
	 *
	 *   @param[in]  mediaType stream type
	 */
	void CheckForDiscontinuityStall(MediaType mediaType);
	
	/**
	 *   @brief Notifies base PTS of the HLS video playback
	 *
	 *   @param[in]  pts - base pts value
	 */
	void NotifyVideoBasePTS(unsigned long long basepts);

	/**
	 *   @brief To get any custom license HTTP headers that was set by application
	 *
	 *   @param[out] headers - map of headers
	 */
	void GetCustomLicenseHeaders(std::unordered_map<std::string, std::vector<std::string>>& customHeaders);

	/**
	 *   @brief Set parallel playlist download config value.
	 *
	 *   @param[in] bValue - true if a/v playlist to be downloaded in parallel
	 *   @return void
	 */
	void SetParallelPlaylistDL(bool bValue);
	/**
	 *   @brief Set async tune configuration
	 *   @param[in] bValue - true if async tune enabled
	 *
	*   @return void
	*/
	void SetAsyncTuneConfig(bool bValue);

	/**
	 *   @brief Get async tune configuration
	 *
	 *   @return bool - true if async tune enabled
	*/
	bool GetAsyncTuneConfig();

	/**
	* @brief Set parallel playlist download config value for linear
	* @param[in] bValue - true if a/v playlist to be downloaded in parallel
	*
	* @return void
	*/
	void SetParallelPlaylistRefresh(bool bValue);

	/**
	 *   @brief Set Westeros sink Configuration
	 *
	 *   @param[in] bValue - true if westeros sink enabled
	 *   @return void
	 */
	void SetWesterosSinkConfig(bool bValue);

	/**
	 *   @brief Set Matching BaseUrl Config Configuration
	 *
	 *   @param[in] bValue - true if Matching BaseUrl enabled
	 *   @return void
	 */
	void SetMatchingBaseUrlConfig(bool bValue);

	/**
	 *	 @brief Configure URI  parameters
	 *	 @param[in] bValue - true to enable, false to disable.
	 *
	 *	 @return void
	 */
	void SetPropagateUriParameters(bool bValue);

	/**
	 *	 @brief Configure New ABR Enable/Disable
	 *	 @param[in] bValue - true if new ABR enabled
	 *
	 *	 @return void
	 */
	void SetNewABRConfig(bool bValue);
	/**
	 *	 @brief Configure New AdBreaker Enable/Disable
	 *	 @param[in] bValue - true if new AdBreaker enabled
	 *
	 *	 @return void
	 */
	void SetNewAdBreakerConfig(bool bValue);

	/**
	 *   @brief To flush buffers in streamsink
	 *
	 *   @param[in] position - position to which we seek after flush
	 *   @param[in] rate - playback rate
	 *   @return void
	 */
	void FlushStreamSink(double position, double rate);

	/**
	 *   @brief Get available audio tracks.
	 *
	 *   @return std::string JSON formatted list of audio tracks
	 */
	std::string GetAvailableAudioTracks();

	/**
	 *   @brief Get available text tracks.
	 *
	 *   @return std::string JSON formatted list of text tracks
	 */
	std::string GetAvailableTextTracks();

	/*
	 *   @brief Get the video window co-ordinates
	 *
	 *   @return current video co-ordinates in x,y,w,h format
	 */
	std::string GetVideoRectangle();
	/**
	 *	 @brief SetPreCacheDownloadList - Function to assign the PreCaching file list
	 *	 @param[in] Playlist Download list	
	 *
	 *	 @return void
	 */
	void SetPreCacheDownloadList(PreCacheUrlList &dnldListInput);	
	/**
	 *   @brief PreCachePlaylistDownloadTask Thread function for PreCaching Playlist 
	 *
	 *   @return void
	 */
	void PreCachePlaylistDownloadTask();

	/*
	 *   @brief Set the application name which has created PlayerInstanceAAMP, for logging purposes
	 *
	 *   @return void
	 */
	void SetAppName(std::string name);

	/*
	*   @brief Get the application name
	*
	*   @return string application name
	*/
	std::string GetAppName();

	/**
	 *   @brief Sends an ID3 metadata event.
	 *
	 *   @param[in] data ID3 metadata
	 */
	void SendId3MetadataEvent(std::vector<uint8_t> &data);

	/**
	 * @brief Gets the registration status of a given event
	 * @param[in] eventType - type of the event to be checked
	 *
	 * @retval bool - True if event is registered
	 */
	bool GetEventListenerStatus(AAMPEventType eventType);

	/**
	 * @brief Check if track can inject data into GStreamer.
	 *
	 * @param[in] Media type
	 * @return bool true if track can inject data, false otherwise
	 */
	bool TrackDownloadsAreEnabled(MediaType type);

	/**
	 * @brief Stop buffering in AAMP and un-pause pipeline.
	 *
	 * @param[in] forceStop - stop buffering forcefully
	 * @return void
	 */
	void StopBuffering(bool forceStop);
	/*
	 *   @brief Check if autoplay enabled for current stream
	 *
	 *   @return true if autoplay enabled
	 */
	bool IsPlayEnabled();

	/**
	 * @brief Soft stop the player instance.
	 *
	 */
	void detach();

	/**
	 * @brief Get pointer to AampCacheHandler
	 *
	 * @return Pointer to AampCacheHandler
	 */
	AampCacheHandler * getAampCacheHandler();

	/*
	 * @brief Set profile ramp down limit.
	 *
	 */
	void SetRampDownLimit(int limit);

	/**
	 * @brief Set Initila profile ramp down limit.
	 *
	 */
	void SetInitRampdownLimit(int limit);

	/**
	 * @brief Set minimum bitrate value.
	 *
	 */
	void SetMinimumBitrate(long bitrate);

	/**
	 * @brief Set maximum bitrate value.
	 *
	 */
	void SetMaximumBitrate(long bitrate);

	/**
	 * @brief Get maximum bitrate value.
	 * @return maximum bitrate value
	 */
	long GetMaximumBitrate();

	/**
	 * @brief Get minimum bitrate value.
	 * @return minimum bitrate value
	 */
	long GetMinimumBitrate();

	/* End AampDrmCallbacks implementation */

	/**
	 *   @brief Set initial buffer duration in seconds
	 *
	 *   @return void
	 */
	void SetInitialBufferDuration(int durationSec);

	/**
	 *   @brief Get current initial buffer duration in seconds
	 *
	 *   @return void
	 */
	int GetInitialBufferDuration();

	/* AampDrmCallbacks implementation */
	/**
	 *   @brief DRM individualization callback
	 *
	 *   @param[in] payload - individualization payload
	 *   @return void
	 */
	void individualization(const std::string& payload);

	/* End AampDrmCallbacks implementation */

	/**
	 *   @brief Set Content Type
	 *
	 *   @param[in]  contentType - Content type
	 *   @return void
	*/
	void SetContentType(const char *contentType);
	/**
	 *   @brief Get Content Type
	 *   @return ContentType
	*/
	ContentType GetContentType() const;

	/**
	 *   @brief Get MediaFormatType
	 *   @return MediaFormatType
	*/

	MediaFormat GetMediaFormatType(const char *url);

	/**
	 * @brief Get license server url for a drm type
	 *
	 * @param[in] type DRM type
	 * @return license server url
	 */
	std::string GetLicenseServerUrlForDrm(DRMSystems type);

	/**
	 *   @brief Set eSTATE_BUFFERING if required
	 *
	 *   @return bool - true if has been set
	 */
	bool SetStateBufferingIfRequired();

	/**
	 *   @brief Check if First Video Frame Displayed Notification
	 *          is required.
	 *
	 *   @return bool - true if required
	 */
	bool IsFirstVideoFrameDisplayedRequired();

	/**
	 *   @brief Notify First Video Frame was displayed
	 *
	 *   @return void
	 */
	void NotifyFirstVideoFrameDisplayed();

	/**
	 *   @brief Set audio track
	 *
	 *   @param[in] trackId - index of audio track in available track list
	 *   @return void
	 */
	void SetAudioTrack(int trackId);

	/**
	 *   @brief Get current audio track index
	 *
	 *   @return int - index of current audio track in available track list
	 */
	int GetAudioTrack();

	/**
	 *   @brief Set text track
	 *
	 *   @param[in] trackId - index of text track in available track list
	 *   @return void
	 */
	void SetTextTrack(int trackId);

	/**
	 *   @brief Get current text track index
	 *
	 *   @return int - index of current text track in available track list
	 */
	int GetTextTrack();

	/**
	 *   @brief Set CC visibility on/off
	 *
	 *   @param[in] enabled - true for CC on, false otherwise
	 *   @return void
	 */
	void SetCCStatus(bool enabled);

	/**
	 *   @brief Switch the subtitle track following a change to the 
	 * 			preferredTextTrack
	 *
	 *   @return void
	 */
	void RefreshSubtitles();

	/**
	 *   @brief Function to notify available audio tracks changed
	 *
	 *   @return void
	 */
	void NotifyAudioTracksChanged();

	/**
	 *   @brief Function to notify available text tracks changed
	 *
	 *   @return void
	 */
	void NotifyTextTracksChanged();

	/**
	 *   @brief Set preferred audio track
	 *   Required to persist across trickplay or other operations
	 *
	 *   @param[in] track - audio track info object
	 *   @return void
	 */
	void SetPreferredAudioTrack(const AudioTrackInfo track) { mPreferredAudioTrack = track; }

	/**
	 *   @brief Set preferred text track
	 *   Required to persist across trickplay or other operations
	 *
	 *   @param[in] track - text track info object
	 *   @return void
	 */
	void SetPreferredTextTrack(const TextTrackInfo track) { mPreferredTextTrack = track; }

	/**
	 *   @brief Get preferred audio track
	 *
	 *   @return AudioTrackInfo - preferred audio track object
	 */
	const AudioTrackInfo &GetPreferredAudioTrack() { return mPreferredAudioTrack; }

	/**
	 *   @brief Get preferred text track
	 *
	 *   @return TextTrackInfo - preferred text track object
	 */
	const TextTrackInfo &GetPreferredTextTrack() { return mPreferredTextTrack; }

	/**
	 *   @brief Set style options for text track rendering
	 *
	 *   @param[in] options - JSON formatted style options
	 *   @return void
	 */
	void SetTextStyle(const std::string &options);

	/**
	 *   @brief Get style options for text track rendering
	 *
	 *   @return std::string - JSON formatted style options
	 */
	std::string GetTextStyle();

	/**
	 *   @brief Check if any active PrivateInstanceAAMP available
	 *
	 *   @return bool true if available
	 */
	static bool IsActiveInstancePresent();

	/**
	 *   @brief Return BasePTS - for non-HLS/TS streams this will be zero
	 *
	 *   @return unsigned long long mVideoBasePTS
	 */
	unsigned long long GetBasePTS() { return mVideoBasePTS; }

	/**
	 *   @brief Set the session Token for player
	 *
	 *   @param[in] string - sessionToken
	 *   @return void
	 */
	void SetSessionToken(std::string &sessionToken);

	/**
	 *   @brief Set stream format for audio/video tracks
	 *
	 *   @param[in] videoFormat - video stream format
	 *   @param[in] audioFormat - audio stream format
	 *   @return void
	 */
	void SetStreamFormat(StreamOutputFormat videoFormat, StreamOutputFormat audioFormat);
	/**
	 *       @brief Set Maximum Cache Size for storing playlist 
	 *       @return void
	*/
	void SetMaxPlaylistCacheSize(int cacheSize);

	/**
	 *   @brief Set video rectangle property
	 *
	 *   @param[in] video rectangle property
	 */
	void EnableVideoRectangle(bool rectProperty);

	/**
	 *   @brief Enable seekable range values in progress event
	 *
	 *   @param[in] enabled - true if enabled
	 */
	void EnableSeekableRange(bool enabled);

	/**
	 *   @brief Enable video PTS reporting in progress event
	 *
	 *   @param[in] enabled - true if enabled
	 */
	void SetReportVideoPTS(bool enabled);

private:

	/**
	 *   @brief Load the configuration lazily
	 *
	 *   @return void
	 */
	void LazilyLoadConfigIfNeeded(void);

	/**
	 *   @brief updates mServiceZone ( service zone) member with string extracted from locator &sz URI parameter
	 *   @param  url - stream url with vss service zone info as query string
	 *   @return std::string
	 */
	void ExtractServiceZone(std::string url);

	/**
	 *   @brief Schedule Event
	 *
	 *   @param[in]  e - Pointer to the event descriptor
	 *   @return void
	 */
	void ScheduleEvent(struct AsyncEventDescriptor* e);

	/**
	 * @brief Deliver all pending Ad events to JSPP
	 *
	 *
	 * @return void
	 */
	void DeliverAdEvents(bool immediate=false);


	/**
	 *   @brief Set Content Type
	 *
	 *   @return string
	 */
	std::string GetContentTypString();

	/**
	 *   @brief Notify about sink buffer full
	 *
	 *   @return void
 	 */
	void NotifySinkBufferFull(MediaType type);
	/**
	 *   @brief Extract DRM init data from the provided URL
	 *          If present, the init data will be removed from the returned URL
	 *          and provided as a separate string
	 *
	 *   @return tuple containing the modified URL and DRM init data
	 */
	const std::tuple<std::string, std::string> ExtractDrmInitData(const char *url);

	/**
	 *   @brief Set local configurations to variables
	 *
	 *   @return void
	 */
	void ConfigureWithLocalOptions();

	ListenerData* mEventListeners[AAMP_MAX_NUM_EVENTS];
	TuneType mTuneType;
	int m_fd;
	bool mIsLive;
	bool mTuneCompleted;
	bool mFirstTune;			//To identify the first tune after load.
	int mfirstTuneFmt;			//First Tune Format HLS(0) or DASH(1)
	int  mTuneAttempts;			//To distinguish between new tune & retries with redundant over urls.
	long long mPlayerLoadTime;
	PrivAAMPState mState;
	long long lastUnderFlowTimeMs[AAMP_TRACK_COUNT];
	long long mLastDiscontinuityTimeMs;
	bool mbTrackDownloadsBlocked[AAMP_TRACK_COUNT];
	std::shared_ptr<AampDrmHelper> mCurrentDrm;
	int  mPersistedProfileIndex;
	long mAvailableBandwidth;
	bool mProcessingDiscontinuity[AAMP_TRACK_COUNT];
	bool mDiscontinuityTuneOperationInProgress;
	ContentType mContentType;
	bool mTunedEventPending;
	bool mSeekOperationInProgress;
	std::map<guint, bool> mPendingAsyncEvents;
	std::unordered_map<std::string, std::vector<std::string>> mCustomHeaders;
	bool mIsFirstRequestToFOG;
	bool mIsLocalPlayback; /** indicates if the playback is from FOG(TSB/IP-DVR) */
	bool mABREnabled;                   /**< Flag that denotes if ABR is enabled */
	long mUserRequestedBandwidth;       /**< preferred bitrate set by user */
	char *mNetworkProxy;                /**< proxy for download requests */
	char *mLicenseProxy;                /**< proxy for license acquisition */
	// VSS license parameters
	std::string mServiceZone; // part of url
	std::string  mVssVirtualStreamId; // part of manifest file

	bool mTrackInjectionBlocked[AAMP_TRACK_COUNT];
#ifdef SESSION_STATS
	CVideoStat * mVideoEnd;
#endif
	std::string  mTraceUUID; // Trace ID unique to tune
	double mTimeToTopProfile;
	double mTimeAtTopProfile;
	unsigned long long mVideoBasePTS;
	double mPlaybackDuration; // Stores Total of duration of VideoDownloaded, it is not accurate playback duration but best way to find playback duration.
	std::unordered_map<std::string, std::vector<std::string>> mCustomLicenseHeaders;
	std::string mAppName;
	PreCacheUrlList mPreCacheDnldList;
	bool mProgressReportFromProcessDiscontinuity; /** flag dentoes if progress reporting is in execution from ProcessPendingDiscontinuity*/

	AampCacheHandler *mAampCacheHandler;
	long mMinBitrate;	/** minimum bitrate limit of profiles to be selected during playback */
	long mMaxBitrate;	/** Maximum bitrate limit of profiles to be selected during playback */
	int mMinInitialCacheSeconds; /**< Minimum cached duration before playing in seconds*/
	std::string mDrmInitData; // DRM init data from main manifest URL (if present)
	std::map<DRMSystems, std::string> mLicenseServerUrls;
	bool mFragmentCachingRequired; /**< True if fragment caching is required or ongoing */
	pthread_mutex_t mFragmentCachingLock; /**< To sync fragment initial caching operations */
	bool mPauseOnFirstVideoFrameDisp; /**< True if pause AAMP after displaying first video frame */
	AudioTrackInfo mPreferredAudioTrack; /**< Preferred audio track from available tracks in asset */
	TextTrackInfo mPreferredTextTrack; /**< Preferred text track from available tracks in asset */
	bool mFirstVideoFrameDisplayedEnabled; /** Set True to enable call to NotifyFirstVideoFrameDisplayed() from Sink */
	int mCacheMaxSize;
	bool mEnableSeekableRange;
	bool mReportVideoPTS;
};
#endif // PRIVAAMP_H

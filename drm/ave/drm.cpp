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
 * @file drm.cpp
 * @brief AVE DRM helper definitions
 */


#if 0
per Doug Adler, following hack(forcing true) allows initialization to complete
rdk\generic\ave\third_party\drm-public\portingkit\robust\CConstraintEnforcer.cpp

IConstraintEnforcer::Status OutputProtectionEnforcer::isConstraintSatisfiedInner(const IOutputProtectionConstraint& opConstraint, bool validateResult)
{
	ACBool result = true;
	ACUns32 error = 0;
	logprintf("%s --> %d\n", __FUNCTION__, __LINE__);
#if 0 // hack
	...
#endif
	return std::pair<ACBool, ACUns32>(result, error);
#endif

// TODO: THIS MODULE NEEDS TO BE MADE MULTI-SESSION-FRIENDLY
#include "drm.h"
#ifdef AVE_DRM
#include "media/IMedia.h" // TBR - remove dependency
#include <sys/time.h>
#include <stdio.h> // for printf
#include <stdlib.h> // for malloc
#include <pthread.h>
#include <errno.h>

using namespace media;

#define DRM_ERROR_SENTINEL_FILE "/tmp/DRM_Error"

#endif /*!NO_AVE_DRM*/

static pthread_mutex_t aveDrmManagerMutex = PTHREAD_MUTEX_INITIALIZER;

#ifdef AVE_DRM

static int drmSignalKeyAquired(void * arg);
static int drmSignalError(void * arg);
static void freeDrmErrorData(void * arg);
/*
From FlashAccessKeyFormats.pdf:
- DRM Key: RSA 1024 bit key pair generated by OEM; public key passed to Adobe
- Machine Key: RSA 1024 bit key pair generated by Adobe Individualization server
- Individualization Transport Key: RSA 1024 bit public key
- Individualization Response Wrapper Key: ARS 128 bit key
- Machine Transport Key: RSA 1024 bit key pair generated by Adobe individualization server
- Session Keys
- Content Keys: AES 128 bit keys

1. Individualization
- done first time device attempts to access protected content
- device and player/runtime identifiers sent to Adobe-hosted server
- each device is assigned machine private key
- Adobe server returns a unique digital machine certificate
- certificate can be revoked to prevent future license acquisition
- persists across reboots as a .bin file

2. Key Negotiation
- client parses manifest, reading URL of local comcast license server (from EXT-X-KEY)
- client extracts initialization vector (IV); placeholder for stronger security (from EXT-X-KEY)
- client collects additional channel-specific DRM metadata from manifest (from EXT-X-FAXS-CM)
- client transmits DRM metadata and machine certificate as parameters to an encrypted license request
- license request is encrypted using Transport public key (from DRM metadata)
- license server returns Content Encryption Key (CEK) and usage rules
- license is signed using license server private key
- license response is signed using Transport private key, and encrypted before being returned to client
- usage rules may authorize offline access
- Flash Player or Adobe AIR runtime client extracts Content Encryption Key (CEK) from license, allowing consumer to view authorized content

3. Decoding/Playback
- Advanced Encryption Standard 128-bit key (AES-128) decryption
- PKCS7 padding
- video output protection enforcement
*/

/**
 * @class TheDRMListener
 * @brief
 */
class TheDRMListener : public MyDRMListener
{
private:
	PrivateInstanceAAMP *mpAamp;
	AveDrm* mpAveDrm;
	int mTrackType;
public:
	/**
	 * @brief TheDRMListener Constructor
	 */
	TheDRMListener(PrivateInstanceAAMP *pAamp, AveDrm* aveDrm,int trackType)
	{
		mpAamp = pAamp;
		mpAveDrm = aveDrm;
		mTrackType = trackType;
		logprintf("TheDRMListener::%s:%d AveDrm[%p]Listner[%p]Track[%d]\n", __FUNCTION__, __LINE__, mpAveDrm,this,mTrackType);
	}

	/**
	 * @brief TheDRMListener Constructor
	 */
	~TheDRMListener()
	{
	}

	/**
	 * @brief Callback on key acquired
	 */
	void SignalKeyAcquired(gint callbackID)
	{
		logprintf("DRMListener::%s:%d[%p][%d]drmState:%d moving to KeyAcquired.callbackID[%d]\n", __FUNCTION__, __LINE__, mpAveDrm,mTrackType, mpAveDrm->mDrmState,callbackID);
		mpAamp->SetCallbackAsDispatched(callbackID);
		mpAveDrm->SetState(eDRM_KEY_ACQUIRED);
		mpAamp->LogDrmInitComplete();
	}

	/**
	 * @brief Callback on initialization success
	 */
	void NotifyInitSuccess()
	{ // callback from successful pDrmAdapter->Initialize
		//log_current_time("NotifyInitSuccess\n");
		gint callbackID = PrivateInstanceAAMP::AddHighIdleTask(drmSignalKeyAquired, this);
		if(callbackID > 0)
		{
			mpAamp->SetCallbackAsPending(callbackID);
		}
	}

	/**
	 * @brief Callback on drm error
	 */
	void NotifyDRMError(uint32_t majorError, uint32_t minorError)//(ErrorCode majorError, DRMMinorError minorError, AVString* errorString, media::DRMMetadata* metadata)
	{
		DRMErrorData *drmerrordata = new DRMErrorData;
		drmerrordata->isRetryEnabled = true;
		drmerrordata->drmFailure = AAMP_TUNE_UNTRACKED_DRM_ERROR;

		switch(majorError)
		{
		case 3329:
			/* Disable retry for error codes above 12000 which are listed as 
			 * Enitilement restriction error codes from license server.
			 * 12000 is recovering on retry so kept retry logic for that.
			 * Exclude the known 4xx errors also from retry, not giving
			 * all 4xx errors since 429 was found to be recovering on retry
			 */
			if(minorError > 12000 || (minorError >= 400 && minorError <= 412))
			{
				drmerrordata->isRetryEnabled = false;
			}
			if(12012 == minorError || 12013 == minorError)
			{
				drmerrordata->drmFailure = AAMP_TUNE_AUTHORISATION_FAILURE;
				snprintf(drmerrordata->description, MAX_ERROR_DESCRIPTION_LENGTH, "AAMP: Authorization failure majorError = %d, minorError = %d",(int)majorError, (int)minorError);
			}
			break;
			
		case 3321:
			/* This was added to avoid the crash in ave drm due to deleting 
			the persistant folder DELIA-34306
			*/
			snprintf(drmerrordata->description, MAX_ERROR_DESCRIPTION_LENGTH, "AAMP: Individualization server down majorerror = %d, minorError = %d",(int)majorError, (int)minorError);
			break;
		case 3322:
		case 3328:
			drmerrordata->drmFailure = AAMP_TUNE_CORRUPT_DRM_DATA;
			
				
			/*
 			 * Creating file "/tmp/DRM_Error" will invoke self heal logic in
			 * ASCPDriver.cpp and regenrate cert files in /opt/persistent/adobe
			 * in the next tune attempt, this could clear tune error scenarios
			 * due to corrupt drm data.
 			*/
			FILE *sentinelFile;
			sentinelFile = fopen(DRM_ERROR_SENTINEL_FILE,"w");
			
			if(sentinelFile)
			{
				fclose(sentinelFile);
			}
			else
			{
				logprintf("DRMListener::%s:%d[%p] : *** /tmp/DRM_Error file creation for self heal failed. Error %d -> %s\n",
				__FUNCTION__, __LINE__, mpAveDrm, errno, strerror(errno));
			}
			
			snprintf(drmerrordata->description, MAX_ERROR_DESCRIPTION_LENGTH, "AAMP: DRM Failure possibly due to corrupt drm data; majorError = %d, minorError = %d",											(int)majorError, (int)minorError);
			break;

		case 3307:
			drmerrordata->drmFailure = AAMP_TUNE_DEVICE_NOT_PROVISIONED;
			drmerrordata->isRetryEnabled = false;
			snprintf(drmerrordata->description, MAX_ERROR_DESCRIPTION_LENGTH, "AAMP: Device not provisioned majorError = %d, minorError = %d",(int)majorError, (int)minorError);
			break;
		case 3314:
			//BCOM-3444
			// Error 3314->Multiple times same Metadata is set to AVE-DRM session or a bad Metadata for a stream set 
			// No need to retune same bad channel or set same meta again. Exit the playback by sending error 
			// Let XRE decide to retune or move to previous good channel
			drmerrordata->drmFailure = AAMP_TUNE_CORRUPT_DRM_METADATA;
			drmerrordata->isRetryEnabled = false;
			snprintf(drmerrordata->description, MAX_ERROR_DESCRIPTION_LENGTH, "AAMP: DRM Metadata error majorError = %d, minorError = %d",(int)majorError, (int)minorError);
			break;
		default:
			break;
		}

		if(AAMP_TUNE_UNTRACKED_DRM_ERROR == drmerrordata->drmFailure)
		{
			snprintf(drmerrordata->description, MAX_ERROR_DESCRIPTION_LENGTH, "AAMP: DRM Failure majorError = %d, minorError = %d",(int)majorError, (int)minorError);
		}
		
		drmerrordata->ptr = this;
		gint callbackID = PrivateInstanceAAMP::AddHighIdleTask(drmSignalError, drmerrordata,freeDrmErrorData);
		if(callbackID > 0)
		{
			mpAamp->SetCallbackAsPending(callbackID);
		}
		logprintf("DRMListener::%s:%d[%p]Track[%d] majorError = %d, minorError = %d drmState:%d\n", __FUNCTION__, __LINE__, mpAveDrm,mTrackType, (int)majorError, (int)minorError, mpAveDrm->mDrmState );
		AAMP_LOG_DRM_ERROR ((int)majorError, (int)minorError);

	}



	/**
	 * @brief Signal drm error
	 */
	void SignalDrmError(AAMPTuneFailure tuneFailure, const char * description, bool isRetryEnabled,gint callbackID)
	{
		mpAamp->DisableDownloads();
		mpAveDrm->SetState(eDRM_KEY_FAILED);
		mpAamp->SendErrorEvent(tuneFailure, description, isRetryEnabled);
		mpAamp->SetCallbackAsDispatched(callbackID);
	}



	/**
	 * @brief Callback on drm status change
	 */
	void NotifyDRMStatus()//media::DRMMetadata* metadata, const DRMLicenseInfo* licenseInfo)
	{ // license available
		logprintf("DRMListener::%s:%d[%p]aamp:***drmState:%d\n", __FUNCTION__, __LINE__, mpAveDrm, mpAveDrm->mDrmState);
	}
};

namespace FlashAccess {

	/**
	 * @brief Caches drm resources
	 */
	void CacheDRMResources();
}


/**
 * @brief Signal key acquired to listener
 * @param arg drm status listener
 * @retval 0
 */
static int drmSignalKeyAquired(void * arg)
{
	gint callbackID = g_source_get_id(g_main_current_source());
	if(callbackID > 0)
	{
		TheDRMListener * drmListener = (TheDRMListener*)arg;
		drmListener->SignalKeyAcquired(callbackID);
	}
	return 0;
}


/**
 * @brief Signal drm error to listener
 * @param arg drm status listener
 * @retval 0
 */
static int drmSignalError(void * arg)
{
	gint callbackID = g_source_get_id(g_main_current_source());
	if(callbackID > 0)
	{
		DRMErrorData *drmerrordata = (DRMErrorData*)arg;
		TheDRMListener * drmListener = (TheDRMListener*)drmerrordata->ptr;
		drmListener->SignalDrmError(drmerrordata->drmFailure,drmerrordata->description,drmerrordata->isRetryEnabled,callbackID);
	}
	return 0;
}


static void freeDrmErrorData(void * arg)
{
	// free the memory when idletask is deleted
	DRMErrorData *drmerrordata = (DRMErrorData*)arg;
	delete drmerrordata;
}

/**
 * @brief SetMetaData Function to create adapter and listner for Metadata handling
 *
 * @param[in] aamp      Pointer to PrivateInstanceAAMP object associated with player
 * @param[in] metadata  Pointed to DrmMetadata structure - unpacked binary metadata from EXT-X-FAXS-CM
 * @param[in] trackType Track Type ( audio / video)
 *
 * @retval eDRM_SUCCESS on success
 */
DrmReturn AveDrm::SetMetaData( class PrivateInstanceAAMP *aamp, void *metadata,int trackType)
{
	const DrmMetadata *drmMetadata = ( DrmMetadata *)metadata;
	pthread_mutex_lock(&mutex);
	mpAamp = aamp;
	DrmReturn err = eDRM_SUCCESS;
	if( !m_pDrmAdapter )
	{
		m_pDrmAdapter = new MyFlashAccessAdapter();
		m_pDrmListner = new TheDRMListener(mpAamp, this,trackType);
		// Need to store the Metadata as part of AveDrm as FragmentCollector frees the Metadata memory
		// inside Flush . This Metadata pointer is referred inside AVE Lib, so its safe to keep a copy within instance
		mMetaData.metadataPtr = new (std::nothrow) unsigned char[drmMetadata->metadataSize];
		mMetaData.metadataSize = drmMetadata->metadataSize;
		memcpy(mMetaData.metadataPtr, drmMetadata->metadataPtr, drmMetadata->metadataSize);
	}

	mDrmState = eDRM_INITIALIZED;
	mPrevDrmState = eDRM_INITIALIZED;
	pthread_mutex_unlock(&mutex);
	logprintf("AveDrm::%s:%d[%p]Track[%d] drmState:%d \n", __FUNCTION__, __LINE__, this,trackType, mDrmState);
	return err;
}

/**
 * @brief StoreDecryptInfoIfChanged Checks if DrmInfo to be stored 
 *
 * @param[in] drmInfo  DRM information required to decrypt
 *
 * @retval True on new DrmInfo , false if already exist
 */
bool AveDrm::StoreDecryptInfoIfChanged( const DrmInfo *drmInfo)
{
	bool newInfo = false;
	if(drmInfo)
	{
		newInfo = true;	
		if(drmInfo->method == mDrmInfo.method)
		{
			if(drmInfo->useFirst16BytesAsIV == mDrmInfo.useFirst16BytesAsIV)
			{
				if(drmInfo->iv && mDrmInfo.iv && (0==memcmp(drmInfo->iv, mDrmInfo.iv,DRM_IV_LEN)))
				{
					if(drmInfo->uri && mDrmInfo.uri && (0==strcmp(drmInfo->uri, mDrmInfo.uri)))
					{
						// Sorry , no luck this time .Have same data here . Ignoring your request
						newInfo = false;
					}
				}
			}
		}
	}
	else
	{
		logprintf("[%s][%d] Invalid DRMInfo provided for SetDecrypto !!! \n",__FUNCTION__,__LINE__);
	}
	if(newInfo)
	{
		// Found new DRM Info , need to set to AVE with decrypt info
		mDrmInfo.method = drmInfo->method;
		mDrmInfo.useFirst16BytesAsIV = drmInfo->useFirst16BytesAsIV;
		if(mDrmInfo.iv)
		{	delete[] mDrmInfo.iv ;mDrmInfo.iv = NULL;}
		if(mDrmInfo.uri)
		{	delete[] mDrmInfo.uri ; mDrmInfo.uri = NULL;}
		if(drmInfo->iv) 
		{	mDrmInfo.iv = new (std::nothrow) unsigned char[DRM_IV_LEN];
			memcpy(mDrmInfo.iv, drmInfo->iv,DRM_IV_LEN);
		}
		if(drmInfo->uri)
		{
			int len = strlen(drmInfo->uri);
			mDrmInfo.uri = new (std::nothrow) char[len+1];
			strncpy(mDrmInfo.uri, drmInfo->uri,len);
			mDrmInfo.uri[len] = 0x0;
		}
	}
	return newInfo;
}


/**
 * @brief Set information required for decryption
 *
 * @param aamp AAMP instance to be associated with this decryptor
 * @param drmInfo DRM information required to decrypt
 * @retval eDRM_SUCCESS on success
 */
DrmReturn AveDrm::SetDecryptInfo( PrivateInstanceAAMP *aamp, const DrmInfo *drmInfo)
{
	DrmReturn err = eDRM_ERROR;
	pthread_mutex_lock(&mutex);
	mpAamp = aamp;

	// find if same DRMInfo is available and already set to Ave-Lib ?? 
	bool bNewDrmInfo = StoreDecryptInfoIfChanged(drmInfo);
	if(bNewDrmInfo)
	{
		if (m_pDrmAdapter)
		{
			// Set the decrypt info within AveDrm , not passed by fragment collector 
			// as memory allocated for iv/uri will be released every playlist refresh
			/// same cannot be passed to Ave-DRM
			m_pDrmAdapter->SetDecryptInfo((const DrmInfo *)&mDrmInfo);
			err = eDRM_SUCCESS;
		}
		else
		{
			logprintf("AveDrm::%s:%d[%p] ERROR -  NULL m_pDrmAdapter\n", __FUNCTION__, __LINE__, this);
		}
	}
	pthread_mutex_unlock(&mutex);
	return eDRM_SUCCESS;
}

/**
 * @brief Decrypts an encrypted buffer
 * @param bucketType Type of bucket for profiling
 * @param encryptedDataPtr pointer to encyrpted payload
 * @param encryptedDataLen length in bytes of data pointed to by encryptedDataPtr
 * @param timeInMs wait time
 */
DrmReturn AveDrm::Decrypt( ProfilerBucketType bucketType, void *encryptedDataPtr, size_t encryptedDataLen,int timeInMs)
{
	DrmReturn err = eDRM_ERROR;

	pthread_mutex_lock(&mutex);
	if (mDrmState == eDRM_ACQUIRING_KEY )
	{
		logprintf( "AveDrm::%s:%d[%p] waiting for key acquisition to complete,wait time:%d\n", __FUNCTION__, __LINE__, this, timeInMs );
		struct timespec ts;
		struct timeval tv;
		gettimeofday(&tv, NULL);
		ts.tv_sec = time(NULL) + timeInMs / 1000;
		ts.tv_nsec = (long)(tv.tv_usec * 1000 + 1000 * 1000 * (timeInMs % 1000));
		ts.tv_sec += ts.tv_nsec / (1000 * 1000 * 1000);
		ts.tv_nsec %= (1000 * 1000 * 1000);

		if(ETIMEDOUT == pthread_cond_timedwait(&cond, &mutex, &ts)) // block until drm ready
		{
			logprintf("AveDrm::%s:%d[%p] wait for key acquisition timed out\n", __FUNCTION__, __LINE__, this);
			err = eDRM_KEY_ACQUSITION_TIMEOUT;
		}
	}

	if (mDrmState == eDRM_KEY_ACQUIRED)
	{
		// create same-sized buffer for decrypted data; q: can we decrypt in-place?
		struct DRMBuffer decryptedData;
		decryptedData.buf = (uint8_t *)malloc(encryptedDataLen);
		if (decryptedData.buf)
		{
			decryptedData.len = (uint32_t)encryptedDataLen;

			struct DRMBuffer encryptedData;
			encryptedData.buf = (uint8_t *)encryptedDataPtr;
			encryptedData.len = (uint32_t)encryptedDataLen;

			mpAamp->LogDrmDecryptBegin(bucketType);
			if( 0 == m_pDrmAdapter->Decrypt(true, encryptedData, decryptedData))
			{
				err = eDRM_SUCCESS;
			}
			mpAamp->LogDrmDecryptEnd(bucketType);

			memcpy(encryptedDataPtr, decryptedData.buf, encryptedDataLen);
			free(decryptedData.buf);
		}
	}
	else
	{
		logprintf( "AveDrm::%s:%d[%p]  aamp:key acquisition failure! mDrmState = %d\n", __FUNCTION__, __LINE__, this, (int)mDrmState);
	}
	pthread_mutex_unlock(&mutex);
	return err;
}


/**
 * @brief Release drm session
 */
void AveDrm::Release()
{
	pthread_mutex_lock(&mutex);
	if (m_pDrmAdapter)
	{
		// close all drm sessions
		m_pDrmAdapter->AbortOperations();
	}
	pthread_cond_broadcast(&cond);
	pthread_mutex_unlock(&mutex);
}


/**
 * @brief Cancel timed_wait operation drm_Decrypt
 */
void AveDrm::CancelKeyWait()
{
	pthread_mutex_lock(&mutex);
	//save the current state in case required to restore later.
	if (mDrmState != eDRM_KEY_FLUSH)
	{
		mPrevDrmState = mDrmState;
	}
	//required for demuxed assets where the other track might be waiting on mutex lock.
	mDrmState = eDRM_KEY_FLUSH;
	pthread_cond_broadcast(&cond);
	pthread_mutex_unlock(&mutex);
}


/**
 * @brief Restore key state post cleanup of
 * audio/video TrackState in case DRM data is persisted
 */
void AveDrm::RestoreKeyState()
{
	pthread_mutex_lock(&mutex);
	//In case somebody overwritten mDrmState before restore operation, keep that state
	if (mDrmState == eDRM_KEY_FLUSH)
	{
		mDrmState = mPrevDrmState;
	}
	pthread_mutex_unlock(&mutex);
}


/**
 * @brief Destructor of AveDrm
 *
 */
AveDrm::~AveDrm()
{
	if(m_pDrmListner)
	{
		delete m_pDrmListner;
		m_pDrmListner = NULL;
	}
	if(m_pDrmAdapter)
	{
		delete m_pDrmAdapter;
		m_pDrmAdapter = NULL;
	}
	if(mMetaData.metadataPtr)
	{
		delete[] mMetaData.metadataPtr;
		mMetaData.metadataPtr = NULL;
	}
	if(mDrmInfo.iv)
	{
		delete[] mDrmInfo.iv;
		mDrmInfo.iv = NULL;
	}
	if(mDrmInfo.uri)
	{
		delete[] mDrmInfo.uri;
		mDrmInfo.uri = NULL;
	}

	pthread_mutex_destroy(&mutex);
	pthread_cond_destroy(&cond);
}

/**
* @brief AcquireKey Acquire key for AveDrm instance for Meta provided
*
* @param[in] aamp      Pointer to PrivateInstanceAAMP object associated with player
* @param[in] metadata  Pointed to DrmMetadata structure - unpacked binary metadata from EXT-X-FAXS-CM
*/
void AveDrm::AcquireKey( class PrivateInstanceAAMP *aamp, void *metadata,int trackType)
{
	(void)metadata;
	pthread_mutex_lock(&mutex);
	mpAamp = aamp;
	if( !m_pDrmAdapter )
	{
		logprintf("%s:%d:%d WARNING !!! KeyAcquisition cannot be done without SetMetadata !!!\n",__FUNCTION__,__LINE__,trackType);
		return;
	}

	mDrmState = eDRM_ACQUIRING_KEY;
	// mMetaData - Memory allocated within AveDrm, which is assigned to Ave-Lib. 
	m_pDrmAdapter->Initialize( (const DrmMetadata *)&mMetaData, m_pDrmListner );
	m_pDrmAdapter->SetupDecryptionContext();

	mPrevDrmState = eDRM_INITIALIZED;
	pthread_mutex_unlock(&mutex);
	logprintf("AveDrm::%s:%d[%p] drmState:%d Track[%d]\n", __FUNCTION__, __LINE__, this, mDrmState,trackType);
}



#else  // for Non-AVE macro

DrmReturn AveDrm::SetMetaData(class PrivateInstanceAAMP *aamp, void *drmMetadata,int trackType)
{
	return eDRM_ERROR;
}

void AveDrm::AcquireKey( class PrivateInstanceAAMP *aamp, void *metadata, int trackType)
{

}

DrmReturn AveDrm::SetDecryptInfo( PrivateInstanceAAMP *aamp, const DrmInfo *drmInfo)
{
	return eDRM_ERROR;
}

DrmReturn AveDrm::Decrypt( ProfilerBucketType bucketType, void *encryptedDataPtr, size_t encryptedDataLen,int timeInMs)
{
	return eDRM_SUCCESS;
}


void AveDrm::Release()
{
}


void AveDrm::CancelKeyWait()
{
}

void AveDrm::RestoreKeyState()
{
}

/**
* @brief Destructor of AveDrm
*
*/
AveDrm::~AveDrm()
{
	pthread_mutex_destroy(&mutex);
	pthread_cond_destroy(&cond);
	if(mMetaData.metadataPtr)
	{
		delete[] mMetaData.metadataPtr;
		mMetaData.metadataPtr = NULL;
	}
	if(mDrmInfo.iv)
	{
		delete[] mDrmInfo.iv;
		mDrmInfo.iv = NULL; 
	}
	if(mDrmInfo.uri)
	{
		delete[] mDrmInfo.uri;
		mDrmInfo.uri = NULL;
	}
}

#endif // !AVE_DRM


/**
 * @brief AveDrm Constructor
 */
AveDrm::AveDrm() : mpAamp(NULL), m_pDrmAdapter(NULL), m_pDrmListner(NULL),
		mDrmState(eDRM_INITIALIZED), mPrevDrmState(eDRM_INITIALIZED)
{
	pthread_mutex_init(&mutex, NULL);
	pthread_cond_init(&cond, NULL);
	mMetaData.metadataPtr = NULL;
	mMetaData.metadataSize = 0;
	mDrmInfo.method = DrmMethod::eMETHOD_NONE;
	mDrmInfo.useFirst16BytesAsIV = false;
	mDrmInfo.iv = NULL;
	mDrmInfo.uri = NULL;
}


/**
 * @brief Set state and signal waiting threads. Used internally by listener.
 *
 * @param state State to be set
 */
void AveDrm::SetState(DRMState state)
{
	pthread_mutex_lock(&mutex);
	mDrmState = state;
	pthread_cond_broadcast(&cond);
	pthread_mutex_unlock(&mutex);
}
/**
 * @brief GetState Function to return current DRM State
 *
 * @retval DRMState
 */
DRMState AveDrm::GetState()
{
	return mDrmState;
}

/**
 * @brief AveDrmManager Constructor.
 *
 */
AveDrmManager::AveDrmManager() :
		mDrm(NULL)
{
	mDrm = std::make_shared<AveDrm>();
	Reset();
}


//#define ENABLE_AVE_DRM_MANGER_DEBUG
#ifdef ENABLE_AVE_DRM_MANGER_DEBUG
#define AVE_DRM_MANGER_DEBUG logprintf
#else
#define AVE_DRM_MANGER_DEBUG traceprintf
#endif

/**
 * @brief Reset state of AveDrmManager instance. Used internally
 */
void AveDrmManager::Reset()
{
	mDrmContexSet = false;
	memset(mSha1Hash, 0, DRM_SHA1_HASH_LEN);
	mUserCount = 0;
	mTrackType = 0;
	mDeferredTime = 0;
	mHasBeenUsed = false;
}

void AveDrmManager::UpdateBeforeIndexList(const char* trackname,int trackType)
{
	pthread_mutex_lock(&aveDrmManagerMutex);
	for (int i = 0; i < sAveDrmManager.size(); i++)
        {
		if(sAveDrmManager[i]->mHasBeenUsed && (sAveDrmManager[i]->mTrackType & (1<<trackType))){
	                sAveDrmManager[i]->mUserCount--;
			sAveDrmManager[i]->mTrackType &= ~(1<<trackType);
		}
        }
	pthread_mutex_unlock(&aveDrmManagerMutex);
}

void AveDrmManager::FlushAfterIndexList(const char* trackname,int trackType)
{
	std::vector<AveDrmManager*>::iterator iter;
	pthread_mutex_lock(&aveDrmManagerMutex);
	for (iter = sAveDrmManager.begin(); iter != sAveDrmManager.end();)
	{
		AveDrmManager* aveDrmManager = *iter;
		if(aveDrmManager->mHasBeenUsed)
		{
			if(aveDrmManager->mUserCount <= 0 )
			{
				logprintf("[%s][%s] Erased unused DRM Metadata.Size remaining=%d \n",__FUNCTION__,trackname,sAveDrmManager.size()-1);
				aveDrmManager->mDrm->Release();
				aveDrmManager->Reset();
				delete aveDrmManager;
				iter = sAveDrmManager.erase(iter);
			}
			else
			{ 	// still more users available.
				// logprintf("[%s][%s] Not removing the hash [%s] with user count[%d]\n",__FUNCTION__,trackname,aveDrmManager->mSha1Hash,aveDrmManager->mUserCount);
				iter++;
			}
		}
		else
		{
			AVE_DRM_MANGER_DEBUG("[%s][%s] aveDrmManager found hash untouched , not removing\n",__FUNCTION__,trackname);
			iter++;
		}

	}
	pthread_mutex_unlock(&aveDrmManagerMutex);
}


/**
 * @brief Reset state of AveDrmManager.
 */
void AveDrmManager::ResetAll()
{
	pthread_mutex_lock(&aveDrmManagerMutex);
	for (int i = 0; i < sAveDrmManager.size(); i++)
	{
		sAveDrmManager[i]->Reset();
	}
	pthread_mutex_unlock(&aveDrmManagerMutex);
}

/**
 * @brief Cancel wait inside Decrypt function of all active DRM instances
 */
void AveDrmManager::CancelKeyWaitAll()
{
	pthread_mutex_lock(&aveDrmManagerMutex);
	for (int i = 0; i < sAveDrmManager.size(); i++)
	{
		sAveDrmManager[i]->mDrm->CancelKeyWait();
	}
	pthread_mutex_unlock(&aveDrmManagerMutex);
}

/**
 * @brief Release all active DRM instances
 */
void AveDrmManager::ReleaseAll()
{
	// Only called from Stop of fragment collector of hls , mutex protection 
	// added for calling 
	
	logprintf("[%s]Releasing AveDrmManager of size=%d \n",__FUNCTION__,sAveDrmManager.size());
	std::vector<AveDrmManager*>::iterator iter;
	pthread_mutex_lock(&aveDrmManagerMutex);
	for (iter = sAveDrmManager.begin(); iter != sAveDrmManager.end();)
	{
		AveDrmManager* aveDrmManager = *iter;

		aveDrmManager->mDrm->Release();
		aveDrmManager->Reset();
		delete aveDrmManager;
		iter = sAveDrmManager.erase(iter);
	}
	pthread_mutex_unlock(&aveDrmManagerMutex);
}

/**
 * @brief Restore key states of all active DRM instances
 */
void AveDrmManager::RestoreKeyStateAll()
{
	pthread_mutex_lock(&aveDrmManagerMutex);
	for (int i = 0; i < sAveDrmManager.size(); i++)
	{
		sAveDrmManager[i]->mDrm->RestoreKeyState();
	}
	pthread_mutex_unlock(&aveDrmManagerMutex);
}

/**
* @brief Set DRM meta-data. Creates AveDrm instance if meta-data is not already configured.
*
* @param[in] aamp          AAMP instance associated with the operation.
* @param[in] metaDataNode  DRM meta data node containing meta-data to be set.
* @param[in] trackType     Source track type (audio/video)
*/
void AveDrmManager::SetMetadata(PrivateInstanceAAMP *aamp, DrmMetadataNode *metaDataNode,int trackType)
{
	AveDrmManager* aveDrmManager = NULL;
	bool drmMetaDataAlreadyStored = false;
	AVE_DRM_MANGER_DEBUG ("%s:%d: Enter sAveDrmManager.size = %d\n", __FUNCTION__, __LINE__, (int)sAveDrmManager.size());
	pthread_mutex_lock(&aveDrmManagerMutex);
	for (int i = 0; i < sAveDrmManager.size(); i++)
	{
		if (0 == memcmp(metaDataNode->sha1Hash, sAveDrmManager[i]->mSha1Hash, DRM_SHA1_HASH_LEN))
		{
			drmMetaDataAlreadyStored = true;
			if(sAveDrmManager[i]->mTrackType & (1<<trackType))
			{
				AVE_DRM_MANGER_DEBUG("[%s][%d] Meta hash[%s] already set for tracktype[%x]UserCount[%d]\n",__FUNCTION__, __LINE__,metaDataNode->sha1Hash,sAveDrmManager[i]->mTrackType,sAveDrmManager[i]->mUserCount);
			}
			else
			{
				sAveDrmManager[i]->mTrackType |= (1<<trackType);
				sAveDrmManager[i]->mUserCount++;
			}
			AVE_DRM_MANGER_DEBUG("%s:%d: Found matching sha1Hash[%s]. Index[%d] UserCount[%d][%x]\n", __FUNCTION__, __LINE__,metaDataNode->sha1Hash, i,sAveDrmManager[i]->mUserCount,sAveDrmManager[i]->mTrackType);
		}
	}
	if (!drmMetaDataAlreadyStored)
	{
		aveDrmManager = new AveDrmManager();
		sAveDrmManager.push_back(aveDrmManager);
		aveDrmManager->mDrmContexSet = true;
		aveDrmManager->mUserCount++;
		aveDrmManager->mTrackType |= (1<<trackType);
		memcpy(aveDrmManager->mSha1Hash, metaDataNode->sha1Hash, DRM_SHA1_HASH_LEN);
		aveDrmManager->mDeferredTime = metaDataNode->drmKeyReqTime;
		aveDrmManager->mDrm->SetMetaData(aamp, &metaDataNode->metaData,trackType);
		logprintf("%s:%d: Created new AveDrmManager[%s] .Track[%d].Total Sz=%d\n", __FUNCTION__, __LINE__,metaDataNode->sha1Hash,trackType,sAveDrmManager.size());
	}
	pthread_mutex_unlock(&aveDrmManagerMutex);
	AVE_DRM_MANGER_DEBUG ("%s:%d: Exit sAveDrmManager.size = %d\n", __FUNCTION__, __LINE__, sAveDrmManager.size());
}


/**
* @brief AcquireKey Acquire key for Meta data provided for stream type
*
* @param[in] aamp      Pointer to PrivateInstanceAAMP object associated with player
* @param[in] metadata  Pointed to DrmMetadata structure - unpacked binary metadata from EXT-X-FAXS-CM
* @param[in] trackType Track type audio / video
* @param[in] overrideDeferring Flag to indicate override deferring and request key immediately
*/
bool AveDrmManager::AcquireKey(PrivateInstanceAAMP *aamp, DrmMetadataNode *metaDataNode,int trackType,bool overrideDeferring)
{
	bool retStatus = true;
	AveDrmManager* aveDrmManager = NULL;
	bool drmMetaFound = false;
	int metaIdx = 0;
	bool drmMetaDataKeyRequested = false;
	// a buffer of 5 sec is considered.As next request will happen after playlist refresh
	long long currTime = aamp_GetCurrentTimeMS() + 5000;
	AVE_DRM_MANGER_DEBUG ("%s:%d: Enter sAveDrmManager.size = %d\n", __FUNCTION__, __LINE__, (int)sAveDrmManager.size());
	//
	pthread_mutex_lock(&aveDrmManagerMutex);
	for (int idx = 0; idx < sAveDrmManager.size(); idx++)
	{
		if (0 == memcmp(metaDataNode->sha1Hash, sAveDrmManager[idx]->mSha1Hash, DRM_SHA1_HASH_LEN))
		{
			// Check if key already acquired '
			DRMState currState = sAveDrmManager[idx]->mDrm->GetState() ;
			if(currState != DRMState::eDRM_ACQUIRING_KEY && currState != DRMState::eDRM_KEY_ACQUIRED)
			{
				// new one - VSS Deferring logic. Check if any time is set
				if(sAveDrmManager[idx]->mDeferredTime && overrideDeferring ==false)
				{
					if(sAveDrmManager[idx]->mDeferredTime < currTime)
					{
						drmMetaDataKeyRequested = true;
					}
				}
				else
				{
					// No deferred time set , request key immediately
					drmMetaDataKeyRequested = true;
				}
			}
			metaIdx = idx;
			drmMetaFound = true;
			break;
		}
	}

	if(drmMetaDataKeyRequested)
	{
		logprintf("[%s][%d][%d] Request KeyIdx[%d] for hash[%s]\n",__FUNCTION__,__LINE__,trackType,metaIdx,metaDataNode->sha1Hash);
		sAveDrmManager[metaIdx]->mDrm->AcquireKey(aamp, &metaDataNode->metaData,trackType);
	}

	if(!drmMetaFound)
	{
		// Something wrong . Key request is Made for the Meta which is not available.
		retStatus = false;
		logprintf("%s:%d:[%d] Key Request made for Meta which is not stored..Mismatched Hash data [%s] ?\n",__FUNCTION__, __LINE__, trackType,metaDataNode->sha1Hash);
	}
	pthread_mutex_unlock(&aveDrmManagerMutex);
	return retStatus;

}
/**
 * @brief Print DRM metadata hash
 *
 * @param sha1Hash SHA1 hash to be printed
 */
void AveDrmManager::PrintSha1Hash(char* sha1Hash)
{
	for (int j = 0; j < DRM_SHA1_HASH_LEN; j++)
	{
		printf("%c", sha1Hash[j]);
	}
	printf("\n");
}

/**
 * @brief Dump cached DRM licenses
 */
void AveDrmManager::DumpCachedLicenses()
{
	std::shared_ptr<AveDrm>  aveDrm = nullptr;
	printf("%s:%d sAveDrmManager.size %d\n", __FUNCTION__, __LINE__, (int)sAveDrmManager.size());
	for (int i = 0; i < sAveDrmManager.size(); i++)
	{
		printf("%s:%d sAveDrmManager[%d] mDrmContexSet %s .mSha1Hash -  ", __FUNCTION__, __LINE__, i,
				sAveDrmManager[i]->mDrmContexSet?"true":"false");
		PrintSha1Hash(sAveDrmManager[i]->mSha1Hash);
	}
	printf("\n");
}

/**
 * @brief Get AveDrm instance configured with a specific metadata
 *
 * @param[in] sha1Hash SHA1 hash of meta-data
 * @param[in] trackType Sourec track type (audio/video)
 *
 * @return AveDrm  Instance corresponds to sha1Hash
 * @return NULL    If AveDrm instance configured with the meta-data is not available
 */
std::shared_ptr<AveDrm> AveDrmManager::GetAveDrm(char* sha1Hash,int trackType)
{
        std::shared_ptr<AveDrm>  aveDrm = nullptr;
        pthread_mutex_lock(&aveDrmManagerMutex);
	// Cases to handle
	// a) sha1Hash is having value - If there are multi key,hash will be present.Easy to map hash to correct meta
	// b) sha1Hash is null - Two scenario possible
	//	1. There is only one Common key for both audio and video track
	//	2. There will be separate keys for audio/video.Match using the trackType

	// Case a
	if(sha1Hash){
        for (int i = 0; i < sAveDrmManager.size(); i++)
        {
                if (sAveDrmManager[i]->mDrmContexSet)
                {
                        if (0 == memcmp(sha1Hash, sAveDrmManager[i]->mSha1Hash, DRM_SHA1_HASH_LEN))
                        {
                                aveDrm = sAveDrmManager[i]->mDrm;
                                // Found a user for the Meta data , update the flag so that it can be removed when no users for it
                                sAveDrmManager[i]->mHasBeenUsed = true;
                                AVE_DRM_MANGER_DEBUG ("%s:%d: Found matching sha1Hash[%s]. Index[%d] aveDrm[%p]\n", __FUNCTION__, __LINE__,sha1Hash, i, aveDrm.get());
                                break;
                        }
                }
                else
                {
                        logprintf("%s:%d:[%d] sHlsDrmContext[%d].mDrmContexSet is false\n", __FUNCTION__, __LINE__,trackType, i);
                }
        }
	}
	// case b.1
	else if(sAveDrmManager.size() == 1)
	{
		if (sAveDrmManager[0]->mDrmContexSet)
		{
			aveDrm = sAveDrmManager[0]->mDrm;
			sAveDrmManager[0]->mHasBeenUsed = true;
			logprintf("%s:%d:[%d] Returned only available Drm Instance \n", __FUNCTION__, __LINE__,trackType);
		}
		else
		{
			logprintf("%s:%d:[%d] sHlsDrmContextmDrmContexSet is false\n", __FUNCTION__, __LINE__,trackType);
		}
	}
	// case b.2
	else if(sAveDrmManager.size() > 1)
	{
		logprintf("%s:%d:[%d] Multi Meta[%d]available  without hash.Matching trackTypee \n", __FUNCTION__, __LINE__,trackType,sAveDrmManager.size());
		for (int i = 0; i < sAveDrmManager.size(); i++)
		{
			logprintf("%s:%d:[%d] Idx[%d] ContextSet[%d] mTractType[%d]\n",__FUNCTION__, __LINE__,trackType,i,sAveDrmManager[i]->mDrmContexSet,sAveDrmManager[i]->mTrackType);
			if (sAveDrmManager[i]->mDrmContexSet && (sAveDrmManager[i]->mTrackType & (1<<trackType)))
			{
				aveDrm = sAveDrmManager[i]->mDrm;
				sAveDrmManager[i]->mHasBeenUsed = true;
				logprintf("%s:%d:[%d] Found Matching Multi Meta drm asset State[%d]\n",__FUNCTION__, __LINE__,trackType,aveDrm->GetState());
				break;
			}
		}
	}

        pthread_mutex_unlock(&aveDrmManagerMutex);
        return aveDrm;
}


/**
 * @brief Get index of drm meta-data which is not yet configured
 *
 * @param drmMetadataIdx Indexed DRM meta-data
 * @param drmMetadataCount Count of meta-data present in the index
 */
int AveDrmManager::IsMetadataAvailable(char* sha1Hash)
{
        int idx = -1;
        pthread_mutex_lock(&aveDrmManagerMutex);
	if(sha1Hash)
	{
		for (int i = 0; i < sAveDrmManager.size(); i++)
		{
			if (0 == memcmp(sha1Hash, sAveDrmManager[i]->mSha1Hash, DRM_SHA1_HASH_LEN))
			{
				idx = i;
				break;
			}
		}
	}
        pthread_mutex_unlock(&aveDrmManagerMutex);
        return idx;
}



std::vector<AveDrmManager*> AveDrmManager::sAveDrmManager;




/**
 * @}
 */


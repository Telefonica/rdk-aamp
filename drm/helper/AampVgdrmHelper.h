/*
 * If not stated otherwise in this file or this component's license file the
 * following copyright and licenses apply:
 *
 * Copyright 2020 RDK Management
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
#ifndef _AAMP_VGDRM_HELPER_H
#define _AAMP_VGDRM_HELPER_H

#include <memory>
#include <fcntl.h>
#include <set>

#include "AampDrmHelper.h"

struct VgdrmInterchangeBuffer {
	uint32_t dataSize;
};

class AampVgdrmHelper : public AampDrmHelper, AAMPMemorySystem
{
public:
	friend class AampVgdrmHelperFactory;

	const uint32_t FIVE_SECONDS = 5000U;

	virtual const std::string& ocdmSystemId() const;

	void createInitData(std::vector<uint8_t>& initData) const;

	bool parsePssh(const uint8_t* initData, uint32_t initDataLen);

	bool isClearDecrypt() const { return true; }

	bool isHdcp22Required() const { return true; }

	uint32_t keyProcessTimeout() const { return FIVE_SECONDS; }

	void getKey(std::vector<uint8_t>& keyID) const;

	virtual int getDrmCodecType() const { return CODEC_TYPE; }

	bool isExternalLicense() const { return true; };

	void generateLicenseRequest(const AampChallengeInfo& challengeInfo, AampLicenseRequest& licenseRequest) const;

	AAMPMemorySystem* getMemorySystem() override { return this; };

	bool encode(const uint8_t *dataIn, uint32_t dataInSz, std::vector<uint8_t>& dataOut) override;

	bool decode(const uint8_t *dataIn, uint32_t dataInSz, uint8_t* dataOut, uint32_t dataOutSz) override;

	virtual const std::string& friendlyName() const override { return FRIENDLY_NAME; };

	AampVgdrmHelper(const struct DrmInfo& drmInfo) : AampDrmHelper(drmInfo) {}
	~AampVgdrmHelper() { }

private:
	static const std::string VGDRM_OCDM_ID;
	const std::string FRIENDLY_NAME{"VGDRM"};
	const int CODEC_TYPE{4};
	const int KEY_ID_OFFSET{12};
	const int KEY_PAYLOAD_OFFSET{14};
	const int BASE_16{16};
	std::string mPsshStr;

	const std::string VGDRM_SHARED_MEMORY_NAME{"/aamp_drm"};
	const int VGDRM_SHARED_MEMORY_CREATE_OFLAGS{O_RDWR | O_CREAT};
	const int VGDRM_SHARED_MEMORY_READ_OFLAGS{O_RDONLY};
	const mode_t VGDRM_SHARED_MEMORY_MODE{ S_IRWXU | S_IRWXG | S_IRWXO };
};

class AampVgdrmHelperFactory : public AampDrmHelperFactory
{
	std::shared_ptr<AampDrmHelper> createHelper(const struct DrmInfo& drmInfo) const;

	void appendSystemId(std::vector<std::string>& systemIds) const;

	bool isDRM(const struct DrmInfo& drmInfo) const;

private:
	const std::string VGDRM_UUID{"A68129D3-575B-4F1A-9CBA-3223846CF7C3"};

	const std::set<std::string> VGDRM_URI_START = {
		"80701500000810",
		"81701500000810",
		"8070110000080C",
		"8170110000080C",
		"8070110000080c",  /* Lowercase version of above to avoid needing to do case insensative comparison  */
		"8170110000080c"
	};
};

#endif //_AAMP_VGDRM_HELPER_H
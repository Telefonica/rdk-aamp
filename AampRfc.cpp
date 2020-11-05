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
 * @file AampRfc.cpp
 * @brief APIs to get RFC configured data
 */

#include <string>
#include <cstdio>
#include "tr181api.h"
#include "AampRfc.h"
#include "GlobalConfigAAMP.h"

namespace RFCSettings
{

#define AAMP_RFC_CALLERID        "aamp"
#define AAMP_LRH_AcceptValue_RFC_PARAM           "Device.DeviceInfo.X_RDKCENTRAL-COM_RFC.aamp.LRHAcceptValue" /*LRH stand for License Request Header */
#define AAMP_LRH_ContentType_RFC_PARAM           "Device.DeviceInfo.X_RDKCENTRAL-COM_RFC.aamp.LRHContentType"
#define AAMP_ZERO_DRM_HOST_URL_RFC_PARAM         "Device.DeviceInfo.X_RDKCENTRAL-COM_RFC.aamp.ZeroDrmHostUrl"
#define AAMP_QA_DRM_LicenseServer_RFC_PARAM      "Device.DeviceInfo.X_RDKCENTRAL-COM_RFC.aamp.QADrmLicenseServer"
#define AAMP_PROD_DRM_LicenseServer_RFC_PARAM      "Device.DeviceInfo.X_RDKCENTRAL-COM_RFC.aamp.ProdDrmLicenseServer"
#define AAMP_Rogers_DRM_LicenseServer_RFC_PARAM      "Device.DeviceInfo.X_RDKCENTRAL-COM_RFC.aamp.RogersDrmLicenseServer"

#ifndef USE_SECCLIENT
#define AAMP_FORMATED_HEADER_BYTES (18) /** Size of https:// + /license + \0 */
    /**
     * @brief   Format URL 
     * @param   Data to be formatted
     * @retval  std::string host value
     */
    std::string formatLicensedUrl(const std::string& input){
        char formated[input.size() + AAMP_FORMATED_HEADER_BYTES];
        sprintf (formated, "https://%s/license", input.c_str());
        return std::string(formated);
    }
#endif

    /**
     * @brief   Fetch data from RFC
     * @param   CallerId and Parameter to be fetched
     * @retval  std::string host value
     */
    std::string getRFCValue(const std::string& parameter){
        TR181_ParamData_t param = {0};
        std::string strhost ;
        tr181ErrorCode_t status = getParam((char*)AAMP_RFC_CALLERID, parameter.c_str(), &param);
        if (tr181Success == status)
        {
            AAMPLOG_INFO("RFC Parameter for %s is %s type = %d", parameter.c_str(), param.value, param.type);
            strhost = std::string(param.value);
        }
        else
        {
            AAMPLOG_ERR("get RFC Parameter for %s Failed : %s type = %d", parameter.c_str(), getTR181ErrorString(status), param.type);
        }    
        return strhost;
    }

    /**
     * @brief   Fetch Zero Drm Host URL  from RFC
     * @param   None
     * @retval  std::string host value
     */
    std::string getZeroDrmHostUrl(){
        return getRFCValue(AAMP_ZERO_DRM_HOST_URL_RFC_PARAM);
    }

    /**
     * @brief   Fetch License Request Header AcceptValue from RFC
     * @param   None
     * @retval  std::string host value
     */
    std::string getLHRAcceptValue(){
        return getRFCValue(AAMP_LRH_AcceptValue_RFC_PARAM);
    }

    /**
     * @brief   Fetch License Request Header ContentType from RFC
     * @param   None
     * @retval  std::string host value
     */
    std::string getLRHContentType(){
        return getRFCValue(AAMP_LRH_ContentType_RFC_PARAM);
    }

    /**
     * @brief   Fetch QA DRM License Server from RFC
     * @param   None
     * @retval  std::string host value
     */
    std::string getQADrmLicenseServer(){
        std::string licenseServer = getRFCValue(AAMP_QA_DRM_LicenseServer_RFC_PARAM);
#ifndef USE_SECCLIENT 
        return formatLicensedUrl(licenseServer);
#else        
        return licenseServer;
#endif
    }

    /**
     * @brief   Fetch DRM License Server from RFC
     * @param   None
     * @retval  std::string host value
     */
    std::string getProdDrmLicenseServer(){
        std::string licenseServer = getRFCValue(AAMP_PROD_DRM_LicenseServer_RFC_PARAM);
#ifndef USE_SECCLIENT 
        return formatLicensedUrl(licenseServer);
#else      
        return licenseServer;
#endif
    }

    /**
     * @brief   Fetch Rogers DRM License Server from RFC
     * @param   None
     * @retval  std::string host value
     */
    std::string getRogersDrmLicenseServer(){
        std::string licenseServer = getRFCValue(AAMP_Rogers_DRM_LicenseServer_RFC_PARAM);
#ifndef USE_SECCLIENT 
        return formatLicensedUrl(licenseServer);
#else        
        return licenseServer;
#endif
    }
}

/**
 * EOF
 */
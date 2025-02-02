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
 * @file ota_shim.h
 * @brief shim for dispatching UVE OTA ATSC playback
 */

#ifndef OTA_SHIM_H_
#define OTA_SHIM_H_

#include "StreamAbstractionAAMP.h"
#include <string>
#include <stdint.h>
#ifdef USE_CPP_THUNDER_PLUGIN_ACCESS
#include <core/core.h>
#include "ThunderAccess.h"
#endif
using namespace std;

/**
 * @class StreamAbstractionAAMP_OTA
 * @brief Fragment collector for OTA
 */
class StreamAbstractionAAMP_OTA : public StreamAbstractionAAMP
{
public:
    StreamAbstractionAAMP_OTA(class PrivateInstanceAAMP *aamp,double seekpos, float rate);
    ~StreamAbstractionAAMP_OTA();
    StreamAbstractionAAMP_OTA(const StreamAbstractionAAMP_OTA&) = delete;
    StreamAbstractionAAMP_OTA& operator=(const StreamAbstractionAAMP_OTA&) = delete;

#ifdef USE_CPP_THUNDER_PLUGIN_ACCESS
    /*Event Handler*/
    void onPlayerStatusHandler(const JsonObject& parameters);
#endif
    void DumpProfiles(void) override;
    void Start() override;
    void Stop(bool clearChannelData) override;
    AAMPStatusType Init(TuneType tuneType) override;
    void GetStreamFormat(StreamOutputFormat &primaryOutputFormat, StreamOutputFormat &audioOutputFormat) override;
    double GetStreamPosition() override;
    MediaTrack* GetMediaTrack(TrackType type) override;
    double GetFirstPTS() override;
    double GetBufferedDuration() override;
    bool IsInitialCachingSupported() override;
    int GetBWIndex(long bitrate) override;
    std::vector<long> GetVideoBitrates(void) override;
    std::vector<long> GetAudioBitrates(void) override;
    long GetMaxBitrate(void) override;
    void StopInjection(void) override;
    void StartInjection(void) override;
    void SeekPosUpdate(double) { };
    void NotifyFirstVideoPTS(unsigned long long pts) { };
    void SetVideoRectangle(int x, int y, int w, int h) override;
    void SetAudioTrack(int index) override;
    void SetAudioTrackByLanguage(const char* lang) override;
    std::vector<AudioTrackInfo> &GetAvailableAudioTracks() override;
    int GetAudioTrack() override;
private:

#ifdef USE_CPP_THUNDER_PLUGIN_ACCESS
    ThunderAccessAAMP thunderAccessObj;
    ThunderAccessAAMP mediaSettingsObj;
    std::string prevState;
    bool tuned;

    ThunderAccessAAMP thunderRDKShellObj;
    bool GetScreenResolution(int & screenWidth, int & screenHeight);
#endif
    void GetAudioTracks();
    void SetPreferredAudioLanguage();
    int GetAudioTrackInternal();
    void NotifyAudioTrackChange(const std::vector<AudioTrackInfo> &tracks);
protected:
    StreamInfo* GetStreamInfo(int idx) override;
};

#endif //OTA_SHIM_H_
/**
 * @}
 */
 



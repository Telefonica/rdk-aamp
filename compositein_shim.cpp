/*
 * If not stated otherwise in this file or this component's license file the
 * following copyright and licenses apply:
 *
 * Copyright 2021 RDK Management
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
 * @file fragmentcollector_COMPOSITEIN.cpp
 * @brief shim for dispatching UVE Composite input playback
 */
#include "compositein_shim.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <assert.h>


#define COMPOSITEINPUT_CALLSIGN "org.rdk.CompositeInput.1"

/**
 * @brief StreamAbstractionAAMP_COMPOSITEIN Constructor
 * @param aamp pointer to PrivateInstanceAAMP object associated with player
 * @param seek_pos Seek position
 * @param rate playback rate
 */
StreamAbstractionAAMP_COMPOSITEIN::StreamAbstractionAAMP_COMPOSITEIN(class PrivateInstanceAAMP *aamp,double seek_pos, float rate)
                             : StreamAbstractionAAMP_VIDEOIN("COMPOSITEIN", COMPOSITEINPUT_CALLSIGN,aamp,seek_pos,rate)
{
	aamp->SetContentType("COMPOSITE_IN");
}

/**
 * @brief StreamAbstractionAAMP_COMPOSITEIN Destructor
 */
StreamAbstractionAAMP_COMPOSITEIN::~StreamAbstractionAAMP_COMPOSITEIN()
{
	AAMPLOG_WARN("%s:%d destructor ",__FUNCTION__,__LINE__);
}
/**
 *   @brief  Starts streaming.
 */
void StreamAbstractionAAMP_COMPOSITEIN::Start(void)
{
	const char *url = aamp->GetManifestUrl().c_str();
	int compositeInputPort = -1;
	if( sscanf(url, "cvbsin://localhost/deviceid/%d", &compositeInputPort ) == 1 )
	{
		StartHelper(compositeInputPort,"startCompositeInput");
	}
}

/**
*   @brief  Stops streaming.
*/
void StreamAbstractionAAMP_COMPOSITEIN::Stop(bool clearChannelData)
{
	StopHelper("stopCompositeInput");
}

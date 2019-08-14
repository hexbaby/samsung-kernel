/*
 * Copyright 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//
// Created by haksung.lyou on 2017-12-06.
// need minimum c++14 compiler
//

#ifndef _JACK__AAUDIO_DRIVER_H_
#define _JACK__AAUDIO_DRIVER_H_

#include <thread>
#include <mutex>
#include <condition_variable>
#include <aaudio/AAudio.h>
#include "HsCirQueue.h"
#include "JackAudioDriver.h"


class DriverInfo {
public:
    DriverInfo():stream(nullptr),frameCnt(0),sampleRate(0),channelCnt(0),framesPerBurst(0),pcmFormat(0),isBigBuffer(false){};
    ~DriverInfo(){};
    AAudioStream* stream;
    int frameCnt;
    int sampleRate;
    int channelCnt;
    int framesPerBurst;
    aaudio_format_t pcmFormat;
    bool isBigBuffer;
};


namespace Jack
{

class AAudioDriver : public JackAudioDriver {
public:
    AAudioDriver(const char* name, const char* alias, JackLockedEngine* engine, JackSynchro* table);
    virtual ~AAudioDriver(){};

	int Open(jack_nframes_t buffe_size,
			 jack_nframes_t samplerate,
			 bool capturing,
			 bool playing,
			 int chan_in,
			 int chan_out,
			 bool monitor,
			 const char* capture_driver_name,
			 const char* playback_driver_name,
			 jack_nframes_t capture_latency,
			 jack_nframes_t playback_latency);

    int Close();
    int Read();
    int Write();
    int Start();
    int Stop();

    aaudio_data_callback_result_t processPlaybackStream(AAudioStream *stream, void *audioData, int32_t numFrames);
    aaudio_data_callback_result_t processPlaybackBigBufferStream(AAudioStream *stream, void *audioData, int32_t numFrames);
    aaudio_data_callback_result_t processRecordingStream(AAudioStream *stream, void *audioData, int32_t numFrames);

private:
    int openPlaybackStream();
    int startPlaybackStream();
    int stopPlaybackStream();
    int closePlaybackStream();

    int openRecordingStream();
    int startRecordingStream();
    int stopRecordingStream();
    int closeRecordingStream();

    void updateDriverInfo(DriverInfo& info);
    void refreshDriver();

    std::mutex mRefreshLock;
    std::mutex mDriverLock;

    std::mutex mRecordingLock;
    std::condition_variable mRecordingCondition;

    std::mutex mPlaybackLock;
    std::condition_variable mPlaybackWriteCondition;

    bool mIsRecordingEnabled;
    float* mpRecordingOnePCM;

    HsCirQueue<float>* mpPlaybackQueue;
    HsCirQueue<float>* mpRecordingQueue;
	
    float* mpPlaybackOnePCM;
    bool mIsFirstPlaybackCB;
    int mQueueItemSize;
    bool mIsRefreshWorking;

public:
    DriverInfo mPlayback;
    DriverInfo mRecording;
    bool mNeedRefresh;
};
}

#endif //_JACK__AAUDIO_DRIVER_H_

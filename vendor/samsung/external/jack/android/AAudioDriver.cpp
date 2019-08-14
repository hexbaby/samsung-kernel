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

#include <cstring>
#include <chrono>
#include "AAudioDriver.h"
#include "JackDriverLoader.h"
#include "JackThreadedDriver.h"
#include "JackEngineControl.h"
#include "JackGraphManager.h"
#include "JackTime.h"
#ifdef ENABLE_JACK_LOGGER
#include "JackLogger.h"
#endif

//#define AAUDIO_TRACE_DEBUG
#ifdef AAUDIO_TRACE_DEBUG
#include "JackDebugger.h"
#endif

namespace Jack
{

AAudioDriver::AAudioDriver(const char* name, const char* alias, JackLockedEngine* engine, JackSynchro* table)
	: JackAudioDriver(name, alias, engine, table){
	
    mIsRecordingEnabled = false;
    mpRecordingOnePCM = nullptr;

    mpPlaybackQueue = nullptr;
    mpRecordingQueue = nullptr;
	
    mpPlaybackOnePCM = nullptr;
    mQueueItemSize = 0;
    mIsFirstPlaybackCB = true;
    mIsRefreshWorking = false;
	mNeedRefresh = false;
    #ifdef AAUDIO_TRACE_DEBUG
    trace_init();
    #endif
}

int AAudioDriver::Open(jack_nframes_t buffer_size,
                              jack_nframes_t samplerate,
                              bool capturing,
                              bool playing,
                              int inchannels,
                              int outchannels,
                              bool monitor,
                              const char* capture_driver_uid,
                              const char* playback_driver_uid,
                              jack_nframes_t capture_latency,
                              jack_nframes_t playback_latency) {
                              
    std::lock_guard<std::mutex> lock(mDriverLock);
    jack_info("AAudioDriver::Open() frameCnt[%d], samplerate[%d], in[%d], out[%d]", buffer_size, samplerate, inchannels, outchannels);

    // Generic JackAudioDriver Open
    if (JackAudioDriver::Open(buffer_size, samplerate, capturing, playing, inchannels, outchannels, monitor,
        capture_driver_uid, playback_driver_uid, capture_latency, playback_latency) != 0) {
        return -1;
    }

    mPlayback.sampleRate = samplerate;
    mPlayback.frameCnt = buffer_size;
    mPlayback.channelCnt = outchannels;

    mRecording.sampleRate = samplerate;
    mRecording.frameCnt = buffer_size;
    mRecording.channelCnt = inchannels;

    if(openPlaybackStream()) return -1;

    if( mRecording.channelCnt > 0 && capturing) {
        mIsRecordingEnabled = true;
        if(openRecordingStream()) return -1;
    }else{
        mIsRecordingEnabled = false;
    }

    return 0;
}

int AAudioDriver::Close() {
    std::lock_guard<std::mutex> lock(mDriverLock);
    jack_info("AAudioDriver::Close()");
    closePlaybackStream();
    if(mIsRecordingEnabled) {
        closeRecordingStream();
    }
	
    JackAudioDriver::Close();
    return 0;
}

int AAudioDriver::Read() {
    std::unique_lock<std::mutex> lk(mRecordingLock);

    #ifdef AAUDIO_TRACE_DEBUG
    trace_begin("R");
    #endif

    if ((nullptr == mpRecordingOnePCM) || (nullptr == mpRecordingQueue)){
        #ifdef AAUDIO_TRACE_DEBUG
        trace_end();
        #endif
        return 0;
    }
    //jack_info("AAudioDriver::Read() items[%d]", mpRecordingQueue->getItemCount());

    int waiting_time = (mPlayback.isBigBuffer)? 300:100;
    if(mpRecordingQueue->getItemCount() <= 0 ) {
        using namespace std::chrono_literals;
        if (std::cv_status::timeout == mRecordingCondition.wait_for(lk, waiting_time * 1ms)) {
            jack_error("AAudioDriver::Read() time out");

            if(mIsRecordingEnabled){
                jack_default_audio_sample_t* inputBuffer_1 = GetInputBuffer(0);
                jack_default_audio_sample_t* inputBuffer_2 = GetInputBuffer(1);
                memset(inputBuffer_1, 0, sizeof(float) * mRecording.frameCnt );
                memset(inputBuffer_2, 0, sizeof(float) * mRecording.frameCnt );
            }

            JackDriver::CycleTakeBeginTime();
            #ifdef AAUDIO_TRACE_DEBUG
            trace_end();
            #endif
            return 0;
        }
    } else if((mpRecordingQueue->getItemCount() > 3) && mIsRecordingEnabled){
        jack_info("AAudioDriver::Read() items[%d] - graph processing under-run or callback over-run", mpRecordingQueue->getItemCount());
        while(mpRecordingQueue->getItemCount() > 1){
            mpRecordingQueue->DeQueue(mpRecordingOnePCM, mQueueItemSize);
        }
    }

    mpRecordingQueue->DeQueue(mpRecordingOnePCM, mQueueItemSize);

    if(mIsRecordingEnabled){
        jack_default_audio_sample_t* inputBuffer_1 = GetInputBuffer(0);
        jack_default_audio_sample_t* inputBuffer_2 = GetInputBuffer(1);
        if( !inputBuffer_1 || !inputBuffer_2 ) return 0;

        for (int i = 0, j = 0; i < mRecording.frameCnt; i++) {
            *(inputBuffer_1 + i) = *(mpRecordingOnePCM + j++);
            if (2 == mRecording.channelCnt) {
                *(inputBuffer_2 + i) = *(mpRecordingOnePCM + j++);
            }
        }
    }else{    // playback only mode, remove all items to wait next condition noti.
        while(mpRecordingQueue->getItemCount() > 0){
            jack_info("AAudioDriver::Read() remove all items in playback only");
            mpRecordingQueue->DeQueue(mpRecordingOnePCM, mQueueItemSize);
        }
    }

    JackDriver::CycleTakeBeginTime();
    #ifdef AAUDIO_TRACE_DEBUG
    trace_end();
    #endif
    return 0;
}

int AAudioDriver::Write() {
    std::unique_lock<std::mutex> lk(mPlaybackLock);
	
    #ifdef AAUDIO_TRACE_DEBUG
    trace_begin("W");
    #endif

    if((nullptr == mpPlaybackQueue) || (nullptr == mpPlaybackOnePCM) ) return 0;

    jack_default_audio_sample_t* outputBuffer_1 = GetOutputBuffer(0);
    jack_default_audio_sample_t* outputBuffer_2 = GetOutputBuffer(1);
    if( !outputBuffer_1 || !outputBuffer_2) return -1;

    for(int i=0, j=0; i < mQueueItemSize;){
        *(mpPlaybackOnePCM + i++) = *(outputBuffer_1 + j);
        *(mpPlaybackOnePCM + i++) = *(outputBuffer_2 + j++);
    }
    mpPlaybackQueue->EnQueue(mpPlaybackOnePCM, mQueueItemSize);

    lk.unlock();
    if(mPlayback.isBigBuffer) mPlaybackWriteCondition.notify_one();

    if(mNeedRefresh){
        refreshDriver();
        mNeedRefresh = false;
    }

    #ifdef AAUDIO_TRACE_DEBUG
    trace_end();
    #endif
    return 0;
}

int AAudioDriver::Start() {
    std::lock_guard<std::mutex> lock(mDriverLock);
    jack_info("AAudioDriver::Start()");

    int res = JackAudioDriver::Start();
    if( res < 0 ){
        JackAudioDriver::Stop();
        return res;
    }
	
    if( mIsRecordingEnabled ){
        if(startRecordingStream()) return -1;
    }
    if(startPlaybackStream()) return -1;
    return 0;
}

int AAudioDriver::Stop() {
    std::lock_guard<std::mutex> lock(mDriverLock);
    jack_info("AAudioDriver::Stop()");

    stopPlaybackStream();
    if( mIsRecordingEnabled ){
        stopRecordingStream();
    }

    int res = JackAudioDriver::Stop();
    if( res < 0 ){
        JackAudioDriver::Stop();
        return -1;
    }
    return 0;
}


/////////////////////  AAudio Function ////////////////////////////////////////////////////

aaudio_data_callback_result_t playbackDataCallback(AAudioStream *stream, void *userData, void *audioData, int32_t numFrames){
    if(nullptr == userData) return AAUDIO_CALLBACK_RESULT_STOP;
    AAudioDriver *driver = reinterpret_cast<AAudioDriver *>(userData);
    if(nullptr == driver->mPlayback.stream) return AAUDIO_CALLBACK_RESULT_STOP;

    #ifdef AAUDIO_TRACE_DEBUG
    trace_begin("W_CB");
    #endif

    aaudio_data_callback_result_t res;
    if( driver->mPlayback.isBigBuffer){
        res = driver->processPlaybackBigBufferStream(stream, audioData, numFrames);
        #ifdef AAUDIO_TRACE_DEBUG
        trace_end();
        #endif
        return res;
    } else {
        res = driver->processPlaybackStream(stream, audioData, numFrames);
        #ifdef AAUDIO_TRACE_DEBUG
        trace_end();
        #endif
        return res;
    }
}

aaudio_data_callback_result_t recordingDataCallback(AAudioStream *stream, void *userData, void *audioData, int32_t numFrames){
    if(nullptr == userData) return AAUDIO_CALLBACK_RESULT_STOP;
    AAudioDriver *driver = reinterpret_cast<AAudioDriver *>(userData);
    if(nullptr == driver->mRecording.stream) return AAUDIO_CALLBACK_RESULT_STOP;

    #ifdef AAUDIO_TRACE_DEBUG
    trace_begin("R_CB");
    #endif

    aaudio_data_callback_result_t res = driver->processRecordingStream(stream, audioData, numFrames);

    #ifdef AAUDIO_TRACE_DEBUG
    trace_end();
    #endif
    return res;
}

void errorCallback(AAudioStream *stream, void *userData, aaudio_result_t error) {
    jack_error("AAudioDriver - errorCallback has result: %s", AAudio_convertResultToText(error));
    if(userData == nullptr) return;
    AAudioDriver *driver = reinterpret_cast<AAudioDriver *>(userData);
    aaudio_stream_state_t streamState = AAudioStream_getState(stream);
    if (streamState == AAUDIO_STREAM_STATE_DISCONNECTED) {
        jack_error("AAudioDriver - ErrorCallback[stream %p] - AAUDIO_STREAM_STATE_DISCONNECTED", stream);
        driver->mNeedRefresh = true;
    }
}
////////////////////////////////////////////////////////////////////////////////

int AAudioDriver::openPlaybackStream() {
    jack_info("AAudioDriver::openPlaybackStream sampleRate[%d], channelCnt[%d], frameCnt[%d]",
              mPlayback.sampleRate, mPlayback.channelCnt, mPlayback.frameCnt);

    // AAudio playback settings
    AAudioStreamBuilder *builder = nullptr;
    aaudio_result_t result = AAudio_createStreamBuilder(&builder);

    if (result == AAUDIO_OK && builder != nullptr) {
        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setSampleRate(builder, mPlayback.sampleRate);
        AAudioStreamBuilder_setChannelCount(builder, mPlayback.channelCnt);
        AAudioStreamBuilder_setFramesPerDataCallback(builder, mPlayback.frameCnt);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT );
        AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setDataCallback(builder, playbackDataCallback, this);
        AAudioStreamBuilder_setErrorCallback(builder, errorCallback, this);

        aaudio_result_t resultStream = AAudioStreamBuilder_openStream(builder, &mPlayback.stream);
        if (resultStream == AAUDIO_OK && mPlayback.stream != nullptr) {
            updateDriverInfo(mPlayback);
        } else {
            jack_error("AAudioDriver::openPlaybackStream Failed [%s]", AAudio_convertResultToText(result));
            return -1;
        }
        AAudioStreamBuilder_delete(builder);
    } else {
        jack_error("AAudioDriver::openPlaybackStream - Unable to obtain an AAudioStreamBuilder object");
        return -1;
    }

    // buffer setting
    std::unique_lock<std::mutex> lk(mPlaybackLock);
	
    mQueueItemSize = mPlayback.channelCnt * mPlayback.frameCnt;
    if( mpPlaybackQueue != nullptr ) delete mpPlaybackQueue;
    mpPlaybackQueue = new HsCirQueue<float>(mQueueItemSize * 100);

    if( mpRecordingQueue != nullptr ) delete mpRecordingQueue;
    mpRecordingQueue = new HsCirQueue<float>(mQueueItemSize * 100);

    if( mpPlaybackOnePCM != nullptr ) delete[] mpPlaybackOnePCM;
    mpPlaybackOnePCM = new float[mQueueItemSize];

    if( mpRecordingOnePCM != nullptr ) delete[] mpRecordingOnePCM;
    mpRecordingOnePCM = new float[mQueueItemSize];

    return 0;
}

int AAudioDriver::startPlaybackStream() {
    jack_info("AAudioDriver::startPlaybackStream");
    if (mPlayback.stream == nullptr) {
        jack_error("AAudioDriver::startPlaybackStream error - stream is null");
        return -1;
    }
    mIsFirstPlaybackCB = true;
    aaudio_result_t result = AAudioStream_requestStart(mPlayback.stream);
    if (result != AAUDIO_OK) {
        jack_error("AAudioDriver::startPlaybackStream error [%s]", AAudio_convertResultToText(result));
        return -1;
    }
    return 0;
}

int AAudioDriver::stopPlaybackStream() {
    jack_info("AAudioDriver::stopPlaybackStream");
    if (mPlayback.stream == nullptr) return -1;

    aaudio_result_t result = AAudioStream_requestStop(mPlayback.stream);
    if (result != AAUDIO_OK) {
        jack_error("AAudioDriver::stopPlaybackStream Error [%s]", AAudio_convertResultToText(result));
        return -1;
    }

    aaudio_stream_state_t inputState = AAUDIO_STREAM_STATE_STOPPING;
    aaudio_stream_state_t nextState = AAUDIO_STREAM_STATE_UNINITIALIZED;
    result = AAudioStream_waitForStateChange(mPlayback.stream, inputState, &nextState, 5000 * 1000);
    if (result != AAUDIO_OK) {
        jack_error("AAudioDriver::stopPlaybackStream AAudioStream_waitForStateChange Error [%s]", AAudio_convertResultToText(result));
        return -1;
    }
    return 0;
}

int AAudioDriver::closePlaybackStream() {

    jack_info("AAudioDriver::closePlaybackStream");
    if (nullptr != mPlayback.stream) {
        aaudio_result_t result = AAudioStream_close(mPlayback.stream);
        if (result != AAUDIO_OK) {
            jack_error("AAudioDriver::closePlaybackStream Error [%s]",
                       AAudio_convertResultToText(result));
        }
	    mPlayback.stream = nullptr;	
    }

    std::unique_lock<std::mutex> lk(mPlaybackLock);
	
    if(nullptr != mpPlaybackQueue){
        delete mpPlaybackQueue;
        mpPlaybackQueue = nullptr;
    }

    if(nullptr != mpRecordingQueue){
        delete mpRecordingQueue;
        mpRecordingQueue = nullptr;
    }

    if(nullptr != mpPlaybackOnePCM){
        delete[] mpPlaybackOnePCM;
        mpPlaybackOnePCM = nullptr;
    }

    if(nullptr != mpRecordingOnePCM){
        delete[] mpRecordingOnePCM;
        mpRecordingOnePCM = nullptr;
    }
	
    return 0;
}

int AAudioDriver::openRecordingStream() {
    jack_info("AAudioDriver::openRecordingStream sampleRate[%d], channelCnt[%d], frameCnt[%d]",
              mRecording.sampleRate, mRecording.channelCnt, mRecording.frameCnt);

    // AAudio recording settings
    AAudioStreamBuilder *builder = nullptr;
    aaudio_result_t result = AAudio_createStreamBuilder(&builder);

    if (result == AAUDIO_OK && builder != nullptr) {
        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setSampleRate(builder, mRecording.sampleRate);
        AAudioStreamBuilder_setChannelCount(builder, mRecording.channelCnt);
        AAudioStreamBuilder_setFramesPerDataCallback(builder, mRecording.frameCnt);
        //AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
        AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setDataCallback(builder, recordingDataCallback, this);
        AAudioStreamBuilder_setErrorCallback(builder, errorCallback, this);

        aaudio_result_t resultStream = AAudioStreamBuilder_openStream(builder, &mRecording.stream);
        if (resultStream == AAUDIO_OK && mRecording.stream != nullptr) {
            updateDriverInfo(mRecording);
        } else {
            jack_error("AAudioDriver::openRecordingStream Failed [%s]", AAudio_convertResultToText(result));
            return -1;
        }
        AAudioStreamBuilder_delete(builder);
    } else {
        jack_error("AAudioDriver::openRecordingStream - Unable to obtain an AAudioStreamBuilder object");
        return -1;
    }

    if( mRecording.isBigBuffer){
        jack_error("AAudioDriver::openRecordingStream - Big buffer recording is not supported.");
        mIsRecordingEnabled = false;
        if (nullptr != mRecording.stream) {
            if (AAUDIO_OK != AAudioStream_close(mRecording.stream)) {
                jack_error("AAudioDriver::closeRecordingStream Error [%s]", AAudio_convertResultToText(result));
            }
            mRecording.stream = nullptr;
        }
        return 0;
    }

    return 0;
}

int AAudioDriver::startRecordingStream() {
    jack_info("AAudioDriver::startRecordingStream");
    if (mRecording.stream == nullptr) {
        jack_error("AAudioDriver::startRecordingStream error - stream is null");
        return -1;
    }
    aaudio_result_t result = AAudioStream_requestStart(mRecording.stream);
    if (result != AAUDIO_OK) {
        jack_error("AAudioDriver::startRecordingStream error [%s]", AAudio_convertResultToText(result));
        return -1;
    }
    return 0;
}

int AAudioDriver::stopRecordingStream() {
    jack_info("AAudioDriver::stopRecordingStream");
    if (mRecording.stream == nullptr) return -1;

    aaudio_result_t result = AAudioStream_requestStop(mRecording.stream);
    if (result != AAUDIO_OK) {
        jack_error("AAudioDriver::stopRecordingStream Error [%s]", AAudio_convertResultToText(result));
        return -1;
    }
    return 0;
}

int AAudioDriver::closeRecordingStream() {
    jack_info("AAudioDriver::closeRecordingStream");
    if (nullptr != mRecording.stream) {
        aaudio_result_t result = AAudioStream_close(mRecording.stream);
        if (result != AAUDIO_OK) {
            jack_error("AAudioDriver::closeRecordingStream Error [%s]",
                       AAudio_convertResultToText(result));
        }
        mRecording.stream = nullptr;
    }

    return 0;
}

void AAudioDriver::updateDriverInfo(DriverInfo& info){
    jack_info("AAudioDriver info #######################");
    aaudio_direction_t  dir = AAudioStream_getDirection(info.stream);
    jack_info("AAudioDriver - Direction: %s", (dir == AAUDIO_DIRECTION_OUTPUT ? "OUTPUT" : "INPUT"));
    jack_info("AAudioDriver - StreamID: %p", info.stream);
    jack_info("AAudioDriver - BufferCapacity: %d", AAudioStream_getBufferCapacityInFrames(info.stream));
    jack_info("AAudioDriver - FramesPerBurst: %d", info.framesPerBurst = AAudioStream_getFramesPerBurst(info.stream));
    (info.framesPerBurst > info.frameCnt)? (info.isBigBuffer = true) : (info.isBigBuffer = false);
    jack_info("AAudioDriver - BigBuffer: %s", (info.isBigBuffer ? "true" : "false"));

    if( true == info.isBigBuffer ){
        AAudioStream_setBufferSizeInFrames(info.stream, info.framesPerBurst * 3);
//    } else if((AAUDIO_DIRECTION_OUTPUT == dir) && (192 == info.frameCnt)){
//        AAudioStream_setBufferSizeInFrames(info.stream, info.framesPerBurst * 2);
    } else {
        AAudioStream_setBufferSizeInFrames(info.stream, info.framesPerBurst);
    }
	
    jack_info("AAudioDriver - BufferSize: %d", AAudioStream_getBufferSizeInFrames(info.stream));
    jack_info("AAudioDriver - XRunCount: %d", AAudioStream_getXRunCount(info.stream));
    jack_info("AAudioDriver - SampleRate: %d", info.sampleRate = AAudioStream_getSampleRate(info.stream));
    jack_info("AAudioDriver - SamplesPerFrame: %d", info.channelCnt = AAudioStream_getChannelCount(info.stream));
    jack_info("AAudioDriver - DeviceId: %d", AAudioStream_getDeviceId(info.stream));

    info.pcmFormat = AAudioStream_getFormat(info.stream);
    std::string  formatDescription;
    switch(info.pcmFormat){
        case AAUDIO_FORMAT_INVALID:
            formatDescription = "AAUDIO_FORMAT_INVALID";
            break;
        case AAUDIO_FORMAT_UNSPECIFIED:
            formatDescription = "AAUDIO_FORMAT_UNSPECIFIED";
            break;
        case AAUDIO_FORMAT_PCM_I16:
            formatDescription = "AAUDIO_FORMAT_PCM_I16";
            break;
        case AAUDIO_FORMAT_PCM_FLOAT:
            formatDescription = "AAUDIO_FORMAT_PCM_FLOAT";
            break;
        default:
            formatDescription = "UNKNOWN";
            break;
    }
    jack_info("AAudioDriver - Format: %s",formatDescription.c_str());
    jack_info("AAudioDriver - SharingMode: %s", AAudioStream_getSharingMode(info.stream) == AAUDIO_SHARING_MODE_EXCLUSIVE ?
                            "EXCLUSIVE" : "SHARED");

    aaudio_performance_mode_t perfMode = AAudioStream_getPerformanceMode(info.stream);
    std::string perfModeDescription;
    switch (perfMode){
        case AAUDIO_PERFORMANCE_MODE_NONE:
            perfModeDescription = "NONE";
            break;
        case AAUDIO_PERFORMANCE_MODE_LOW_LATENCY:
            perfModeDescription = "LOW_LATENCY";
            break;
        case AAUDIO_PERFORMANCE_MODE_POWER_SAVING:
            perfModeDescription = "POWER_SAVING";
            break;
        default:
            perfModeDescription = "UNKNOWN";
            break;
    }
    jack_info("AAudioDriver - PerformanceMode: %s", perfModeDescription.c_str());

    if (dir == AAUDIO_DIRECTION_OUTPUT) {
        jack_info("AAudioDriver - FramesReadByDevice: %d", (int32_t)AAudioStream_getFramesRead(info.stream));
        jack_info("AAudioDriver - FramesWriteByApp: %d", (int32_t)AAudioStream_getFramesWritten(info.stream));
    } else {
        jack_info("AAudioDriver - FramesReadByApp: %d", (int32_t)AAudioStream_getFramesRead(info.stream));
        jack_info("AAudioDriver - FramesWriteByDevice: %d", (int32_t)AAudioStream_getFramesWritten(info.stream));
    }
    return;
}

void AAudioDriver::refreshDriver(){
    if(mRefreshLock.try_lock()){
        using namespace std::chrono_literals;
        std::lock_guard<std::mutex> lock(mDriverLock);
        jack_info("AAudioDriver ++ do refreshDriver ++");
        mIsRefreshWorking = true;

        stopPlaybackStream();
        if(mIsRecordingEnabled) stopRecordingStream();
        std::this_thread::sleep_for(2s);

        closePlaybackStream();
        openPlaybackStream();

        if(mIsRecordingEnabled) {
            closeRecordingStream();
            openRecordingStream();
            startRecordingStream();
            std::this_thread::sleep_for(100ms);
        }
		
        startPlaybackStream();
        mIsRefreshWorking = false;
        jack_info("AAudioDriver -- do refreshDriver --");
        mRefreshLock.unlock();
    }else{
        jack_info("AAudioDriver - refreshDriver is already in progress - ignoring this request");
    }
}

aaudio_data_callback_result_t AAudioDriver::processPlaybackStream(AAudioStream *stream, void *audioData, int32_t numFrames) {
    std::unique_lock<std::mutex> lk(mPlaybackLock);

    if( nullptr == mpPlaybackQueue || nullptr == mpPlaybackOnePCM ){
        jack_error("AAudioDriver::processPlaybackStream - playback buffer is null");
        return AAUDIO_CALLBACK_RESULT_STOP;
    }

    if(!mIsRecordingEnabled){    // Playback only mode
        std::unique_lock<std::mutex> recording_lk(mRecordingLock);
        mpRecordingQueue->EnQueue(mpPlaybackOnePCM,mQueueItemSize);
        recording_lk.unlock();
        mRecordingCondition.notify_one();
    }

    if( mIsFirstPlaybackCB ){
        mIsFirstPlaybackCB = false;
        while(mpPlaybackQueue->getItemCount() > 0){
            mpPlaybackQueue->DeQueue(mpPlaybackOnePCM, mQueueItemSize);
        }
    }

    if(mpPlaybackQueue->getItemCount() <= 0 ){
        memset(audioData, 0, sizeof(float) * mQueueItemSize);
        jack_error("AAudioDriver::processPlaybackStream Queue[%d] <= 0, so skip this callback", mpPlaybackQueue->getItemCount());
		return AAUDIO_CALLBACK_RESULT_CONTINUE;
    } else if (mpPlaybackQueue->getItemCount() > 3  ){
        jack_error("AAudioDriver::processPlaybackStream Queue[%d] > 3, Do dequeue one item", mpPlaybackQueue->getItemCount());
            mpPlaybackQueue->DeQueue(mpPlaybackOnePCM, mQueueItemSize);
    }

    //jack_info("AAudioDriver::processPlaybackStream  Queue[%d], numFrames[%d]", mpPlaybackQueue->getItemCount(), numFrames);
    mpPlaybackQueue->DeQueue(static_cast<float *>(audioData), mQueueItemSize);

    if( mIsRefreshWorking ){
        memset(audioData, 0, sizeof(float)*numFrames);
    }

    #ifdef ENABLE_JACK_LOGGER
    if((NULL != mJackLogger) && (mJackLogger->isStartedPcmDump())){
        mJackLogger->dumpWriteData(static_cast<float *>(audioData), mQueueItemSize);
    }
    #endif
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

aaudio_data_callback_result_t AAudioDriver::processPlaybackBigBufferStream(AAudioStream *stream, void *audioData, int32_t numFrames){
    std::unique_lock<std::mutex> lk(mPlaybackLock);
    if( nullptr == mpPlaybackQueue || nullptr == mpPlaybackOnePCM ){
        jack_error("AAudioDriver::processPlaybackStream - playback buffer is null");
        return AAUDIO_CALLBACK_RESULT_STOP;
    }

    if(!mIsRecordingEnabled){    // Playback only mode
        std::unique_lock<std::mutex> recording_lk(mRecordingLock);
        mpRecordingQueue->EnQueue(mpPlaybackOnePCM,mQueueItemSize);
        recording_lk.unlock();
        mRecordingCondition.notify_one();
    }

    if( mIsFirstPlaybackCB ){
        mIsFirstPlaybackCB = false;
        while(mpPlaybackQueue->getItemCount() > 1){
            mpPlaybackQueue->DeQueue(mpPlaybackOnePCM, mQueueItemSize);
        }
    }

    if( mpPlaybackQueue->getItemCount() <= 0 ){
	    using namespace std::chrono_literals;
        if (std::cv_status::timeout == mPlaybackWriteCondition.wait_for(lk, 40ms)) {  // noti from Write()
            jack_error("AAudioDriver::processPlaybackBigBufferStream Write waiting timeout");
            aaudio_stream_state_t result  = AAudioStream_getState(mPlayback.stream);
            jack_error("AAudioDriver::processPlaybackBigBufferStream state[%d]", result);
            if( result == AAUDIO_STREAM_STATE_STOPPING
                || result == AAUDIO_STREAM_STATE_STOPPED
                || result == AAUDIO_STREAM_STATE_CLOSING
                || result == AAUDIO_STREAM_STATE_CLOSED ){
                return AAUDIO_CALLBACK_RESULT_STOP;
            }
            return AAUDIO_CALLBACK_RESULT_CONTINUE;
        }
    }

    //jack_info("AAudioDriver::processPlaybackBigBufferStream - Queue[%d], numFrames[%d]", mpPlaybackQueue->getItemCount(), numFrames);
    mpPlaybackQueue->DeQueue(static_cast<float *>(audioData), mQueueItemSize);

    if( mIsRefreshWorking ){
        memset(audioData, 0, sizeof(float)*numFrames);
    }

    #ifdef ENABLE_JACK_LOGGER
    if((NULL != mJackLogger) && (mJackLogger->isStartedPcmDump())){
        mJackLogger->dumpWriteData(static_cast<float *>(audioData), mQueueItemSize);
    }
    #endif
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

aaudio_data_callback_result_t AAudioDriver::processRecordingStream(AAudioStream *stream, void *audioData, int32_t numFrames) {
    std::unique_lock<std::mutex> lk(mRecordingLock);
    if( nullptr == mpRecordingOnePCM ){
        jack_error("AAudioDriver::processRecordingStream - recording buffer is null");
        return AAUDIO_CALLBACK_RESULT_STOP;
    }

    //jack_info("AAudioDriver::processRecordingStream numFrames[%d]", numFrames);
    if( AAUDIO_FORMAT_PCM_I16 == mRecording.pcmFormat ){  // change short format to float format pcm.
        for (int i= 0; i < mRecording.frameCnt * mRecording.channelCnt; i++) {
            *(mpRecordingOnePCM + i) = (static_cast<float>(*(static_cast<short *>(audioData) + i))) * 0.000030517578125f;
            if (*(mpRecordingOnePCM + i) > 1) {
                (*(mpRecordingOnePCM + i)) = 1;
            } else if (*(mpRecordingOnePCM + i) < -1) {
                (*(mpRecordingOnePCM + i)) = -1;
            }
        }
    } else if(AAUDIO_FORMAT_PCM_FLOAT == mRecording.pcmFormat ) {
        memcpy(mpRecordingOnePCM, audioData, sizeof(float) * mQueueItemSize);
    } else {
        jack_error("AAudioDriver::processRecordingStream - undefined recording format");
        memset(mpRecordingOnePCM, 0, sizeof(float) * mQueueItemSize);
    }

    if( nullptr != mpRecordingQueue ) mpRecordingQueue->EnQueue(mpRecordingOnePCM, mQueueItemSize);
	
    #ifdef ENABLE_JACK_LOGGER
    if((NULL != mJackLogger) && (mJackLogger->isStartedPcmDump())){
        mJackLogger->dumpReadData(static_cast<float *>(mpRecordingOnePCM), mQueueItemSize);
    }
    #endif

    lk.unlock();
    mRecordingCondition.notify_one();
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
}
///////////// End of AAudio Function  /////////////////////////////////////////////////

#ifdef __cplusplus
extern "C"
{
#endif

    SERVER_EXPORT jack_driver_desc_t * driver_get_descriptor () {
        jack_driver_desc_t * desc;
        jack_driver_desc_filler_t filler;
        jack_driver_param_value_t value;

        desc = jack_driver_descriptor_construct("aaudio", JackDriverMaster, "Timer based backend", &filler);

        value.ui = 2U;
        jack_driver_descriptor_add_parameter(desc, &filler, "capture", 'C', JackDriverParamUInt, &value, NULL, "Number of capture ports", NULL);
        jack_driver_descriptor_add_parameter(desc, &filler, "playback", 'P', JackDriverParamUInt, &value, NULL, "Number of playback ports", NULL);

        value.ui = 48000U;
        jack_driver_descriptor_add_parameter(desc, &filler, "rate", 'r', JackDriverParamUInt, &value, NULL, "Sample rate", NULL);

        value.i = 0;
        jack_driver_descriptor_add_parameter(desc, &filler, "monitor", 'm', JackDriverParamBool, &value, NULL, "Provide monitor ports for the output", NULL);

        value.ui = 384;
        jack_driver_descriptor_add_parameter(desc, &filler, "period", 'p', JackDriverParamUInt, &value, NULL, "Frames per period", NULL);

        value.ui = 2U;
        jack_driver_descriptor_add_parameter(desc, &filler, "nperiods", 'n', JackDriverParamUInt, &value, NULL, "Number of periods of playback latency", NULL);

        value.ui = 21333U;
        jack_driver_descriptor_add_parameter(desc, &filler, "wait", 'w', JackDriverParamUInt, &value, NULL, "Number of usecs to wait between engine processes", NULL);

        return desc;
    }

    SERVER_EXPORT Jack::JackDriverClientInterface* driver_initialize(Jack::JackLockedEngine* engine, Jack::JackSynchro* table, const JSList* params) {
        jack_nframes_t sample_rate = 48000;
        jack_nframes_t buffer_size = 384;
        unsigned int capture_ports = 0;
        unsigned int playback_ports = 2;
        unsigned int user_nperiods = 2;
        const JSList * node;
        const jack_driver_param_t * param;
        bool monitor = false;

        for (node = params; node; node = jack_slist_next (node)) {
            param = (const jack_driver_param_t *) node->data;

            switch (param->character) {

                case 'C':
                    capture_ports = param->value.ui;
                    break;

                case 'P':
                    playback_ports = param->value.ui;
                    break;

                case 'r':
                    sample_rate = param->value.ui;
                    break;

                case 'p':
                    buffer_size = param->value.ui;
                    break;

                case 'n':
                    user_nperiods = param->value.ui;
                    break;

                case 'm':
                    monitor = param->value.i;
                    break;
            }
        }

        Jack::AAudioDriver* aaudio_driver = new Jack::AAudioDriver("system", "aaudio_pcm", engine, table);

        Jack::JackDriverClientInterface* driver = new Jack::JackThreadedDriver(aaudio_driver);
        if (driver->Open(buffer_size, sample_rate, capture_ports? 1 : 0, playback_ports? 1 : 0, capture_ports, playback_ports, monitor, "aaudio", "aaudio", 0, 0) == 0) {
            return driver;
        } else {
            delete driver;
            return NULL;
        }
    }

#ifdef __cplusplus
}
#endif

//  License:
//
//  This source code is provided as is, without warranty.
//  You may copy and distribute verbatim copies of this document.
//  You may modify and use this source code to create binary code for your own purposes, free or commercial.
//

#include "OpenSLDriver.h"
#include "JackDriverLoader.h"
#include "JackThreadedDriver.h"
#include "JackEngineControl.h"
#include "JackGraphManager.h"
#include "JackTime.h"

#include <iostream>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <android/log.h>
#include <time.h>

#ifdef OPENSL_TRACE_DEBUG
#include "JackDebugger.h"
#endif

#ifdef ENABLE_JACK_LOGGER
#include "JackLogger.h"
#endif

//#define OPENSL_TRACE_DEBUG

#define SHORT_TO_FLOAT(s, f)\
	(f) = float(s) * 0.000030517578125f ;\
	if((f) > 1 ) (f) = 1; \
	else if ((f) < -1) (f) = -1;

#define FLOAT_TO_SHORT(f, s)\
	if ((f) <= -1.0f) {\
		(s) = -32767;\
	} else if ((f) >= 1.0f) {\
		(s) = 32767;\
	} else {\
		(s) = lrintf ((f) * 32767.0f);\
	}

#define XRUN_CNT_LIMIT 3


namespace Jack
{

OpenSLDriver::OpenSLDriver(const char* name, const char* alias, JackLockedEngine* engine, JackSynchro* table)
		: JackAudioDriver(name, alias, engine, table){

	engineObject = NULL;
    engineEngine = NULL;
   	outputMixObject = NULL;

	bqPlayerObject = NULL;
   	bqPlayerPlay = NULL;
   	bqPlayerBufferQueue = NULL;
	bqPlayerVolume = NULL;
	
	recorderObject = NULL;
	recorderRecord = NULL;
   	recorderBufferQueue = NULL;

	mIsPlayback = true;
	mIsCapture = false;
	mSampleRate = 48000;
	mFrameSize = 0;
	mOutChannelCnt = 0;
	mInChannelCnt = 0;
	mDriverInChannelCnt = 1; // mono always
	mOutBufSize = 0;
	mInBufSize = 0;
	mOutBufLatency = 8;

	mpOutQueue = NULL;
	mpInQueue = NULL;
	
	mpPCM_OneFrame = NULL;
	mpPCM_MutedOneFrame = NULL;

	mIsOutStarted = false;
	mIsOutCBStarted = false;
	mIsInCBStarted = false;
	mIsFirstRead = true;
	mOutJitterCount = 2;

	mPrevOutCBTime = 0;
	mPrevInCBTime = 0;
   	mCurOutCBTime = 0;
   	mCurInCBTime = 0;

	mNeedDriverRefresh = false;
	mUnderRunCnt = 0;
	mOverRunCnt = 0;

	mpInBuf = NULL;
	mpOutBuf = NULL;
	mpReadBuf = NULL;

	mIsBigBufferPlayback = false;
   	mBigBufferCheckCnt = 0;
   	mBigBufferCBCnt = 0;
	mIsBigBufferChecked = false;

	mBTFakePlayCnt = 0;

	mDriverControlMutex = PTHREAD_MUTEX_INITIALIZER;
	pthread_mutex_init(&mDriverControlMutex, (pthread_mutexattr_t*) NULL);
	mInMutex = PTHREAD_MUTEX_INITIALIZER;
	mInCond = PTHREAD_COND_INITIALIZER;
	mOutMutex = PTHREAD_MUTEX_INITIALIZER;
	mOutCond = PTHREAD_COND_INITIALIZER;

#ifdef OPENSL_TRACE_DEBUG
	trace_init();
#endif
}


OpenSLDriver::~OpenSLDriver(){

	pthread_mutex_destroy(&mDriverControlMutex);
	if( NULL != engineObject ){
		Close();
	}
}


int OpenSLDriver::Open(jack_nframes_t buffer_size,
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

    SLresult result = SL_RESULT_SUCCESS;

	pthread_mutex_lock(&mDriverControlMutex);

    // Generic JackAudioDriver Open
    if (JackAudioDriver::Open(buffer_size, samplerate, capturing, playing, inchannels, outchannels, monitor,
        capture_driver_uid, playback_driver_uid, capture_latency, playback_latency) != 0) {
       	pthread_mutex_unlock(&mDriverControlMutex);
        return -1;
    }

	mIsPlayback = playing;
	mIsCapture = capturing;
	mSampleRate = samplerate;
	mOutChannelCnt = outchannels;
	mInChannelCnt = inchannels;
	mFrameSize = buffer_size;
	mOutBufLatency = (mFrameSize * 1000) / mSampleRate;
	mDriverInChannelCnt = 1;

	mOutBufSize = mFrameSize * mOutChannelCnt;
	mInBufSize = mFrameSize * mDriverInChannelCnt;

    jack_info("OpenSLDriver::Open bufSize[%d], samplerate[%d], capture[%d], play[%d], jitter[%d], OpenSL_capture[%d]",
		buffer_size, samplerate, inchannels, outchannels, mOutJitterCount, mDriverInChannelCnt);

	if (pthread_mutex_init(&mInMutex, (pthread_mutexattr_t*) NULL) != 0) {
	    jack_error("OpenSLDriver::Open pthread_mutex_init failed[%d]", result);		
		return SL_RESULT_UNKNOWN_ERROR;
	}
	
	if (pthread_cond_init(&mInCond, (pthread_condattr_t*) NULL) != 0) {
	    jack_error("OpenSLDriver::Open pthread_cond_init failed[%d]", result);		
		return SL_RESULT_UNKNOWN_ERROR;
	}

	if (pthread_mutex_init(&mOutMutex, (pthread_mutexattr_t*) NULL) != 0) {
	    jack_error("OpenSLDriver::Open pthread_mutex_init failed[%d]", result);		
		return SL_RESULT_UNKNOWN_ERROR;
	}
	
	if (pthread_cond_init(&mOutCond, (pthread_condattr_t*) NULL) != 0) {
	    jack_error("OpenSLDriver::Open pthread_cond_init failed[%d]", result);		
		return SL_RESULT_UNKNOWN_ERROR;
	}

	// Create OpenSL Engine
	result = createEngine();

	if( result != SL_RESULT_SUCCESS) {
	    jack_error("OpenSLDriver::Open createEngine failed[%d]", result);
       	pthread_mutex_unlock(&mDriverControlMutex);
		return -1;
	}

	// In Create
    if (mIsCapture) {
		result = createAudioRecorder();
		if( result != SL_RESULT_SUCCESS) {
		    jack_error("OpenSLDriver::Open createAudioRecorder failed[%d]", result);
	       	pthread_mutex_unlock(&mDriverControlMutex);
			return -1;
		}
    }
/*
	// In Start
	if(mIsCapture){
		result = startAudioRecording();
		if( result != SL_RESULT_SUCCESS) {
		    jack_error("OpenSLDriver::Open startAudioRecording failed[%d]", result);		
			pthread_mutex_unlock(&mDriverControlMutex);
			return -1;
		}
	}
*/
	// Out Create
    if (mIsPlayback) {
		result = createBufferQueueAudioPlayer();
		if( result != SL_RESULT_SUCCESS) {
		    jack_error("OpenSLDriver::Open createBufferQueueAudioPlayer failed[%d]", result);
			pthread_mutex_unlock(&mDriverControlMutex);
			return -1;
		}
    }
/*
	// Out Start
	if(mIsPlayback){
		result = startBufferQueueAudioPlayer();
		if( result != SL_RESULT_SUCCESS) {
		    jack_error("OpenSLDriver::Open startBufferQueueAudioPlayer failed[%d]", result);		
			pthread_mutex_unlock(&mDriverControlMutex);
			return -1;
		}
	}
*/
	pthread_mutex_unlock(&mDriverControlMutex);
    return 0;
}

int OpenSLDriver::Close() {

	pthread_mutex_lock(&mDriverControlMutex);
    jack_info("OpenSLDriver::Close");

    int res = JackAudioDriver::Close();

	deleteAudioRecorder();
	deleteBufferQueueAudioPlayer();
	deleteEngine();

	pthread_mutex_lock(&mInMutex);
	pthread_cond_signal(&mInCond);
	pthread_mutex_unlock(&mInMutex);

	pthread_cond_destroy(&mInCond);
	pthread_mutex_destroy(&mInMutex);
	mInMutex = PTHREAD_MUTEX_INITIALIZER;
	mInCond = PTHREAD_COND_INITIALIZER;

	pthread_mutex_lock(&mOutMutex);
	pthread_cond_signal(&mOutCond);
	pthread_mutex_unlock(&mOutMutex);

	pthread_cond_destroy(&mOutCond);
	pthread_mutex_destroy(&mOutMutex);
	mOutMutex = PTHREAD_MUTEX_INITIALIZER;
	mOutCond = PTHREAD_COND_INITIALIZER;

	pthread_mutex_unlock(&mDriverControlMutex);

    return res;
}

int OpenSLDriver::Read() {

#ifdef OPENSL_TRACE_DEBUG
		trace_begin("READ");
#endif

	if( mpInQueue == NULL ) return 0;
	if( mpReadBuf == NULL ) return 0;

	if( mIsFirstRead == true){
		while(mpInQueue->getItemCount() > 1 ){
			mpInQueue->DeQueue(mpReadBuf, mInBufSize);
		}
		mIsFirstRead = false;
	}

	int refreshTime = 100;
	if( (true == mIsBigBufferPlayback) && !mInChannelCnt){
		refreshTime = 1000;
	}

	pthread_mutex_lock(&mInMutex);
	if( mpInQueue->getItemCount() > 0 ){
		mpInQueue->DeQueue(mpReadBuf, mInBufSize);
	}else{
		if( 0 != timedWait(&mInCond, &mInMutex, refreshTime) ){
			memset(mpReadBuf, 0, sizeof(short) * mInBufSize);
			jack_error("OpenSLDriver::Read() timedWait error");
			if( true == mIsInCBStarted ){
				mNeedDriverRefresh = true;
			}
		} else{
			mpInQueue->DeQueue(mpReadBuf, mInBufSize);
		}
	}
	pthread_mutex_unlock(&mInMutex);

	if( !mInChannelCnt){
#ifdef OPENSL_TRACE_DEBUG
		trace_end();
#endif
		return 0;
	}

    jack_default_audio_sample_t* inputBuffer_1 = GetInputBuffer(0);
    jack_default_audio_sample_t* inputBuffer_2 = GetInputBuffer(1);

    if( inputBuffer_1 == NULL ) return -1;
    if( inputBuffer_2 == NULL ) return -1;

	for(int i=0, j=0 ; i < mFrameSize * mDriverInChannelCnt; ){

		if( 2 == mDriverInChannelCnt ){
			SHORT_TO_FLOAT(*(mpReadBuf + i), *(inputBuffer_1 + j));
			i++;
			SHORT_TO_FLOAT(*(mpReadBuf + i), *(inputBuffer_2 + j));
			i++;
			j++;
		}else{
			SHORT_TO_FLOAT(*(mpReadBuf + i), *(inputBuffer_1 + i));
			SHORT_TO_FLOAT(*(mpReadBuf + i), *(inputBuffer_2 + i));
			i++;
		}
	}

#ifdef ENABLE_JACK_LOGGER
    if((NULL != mJackLogger) && (mJackLogger->isStartedPcmDump())){
        mJackLogger->dumpReadData((float*)inputBuffer_1, mFrameSize);
    }
#endif    

#ifdef OPENSL_TRACE_DEBUG
	trace_end();
#endif
    return 0;
}


int OpenSLDriver::Write() {

#ifdef OPENSL_TRACE_DEBUG
	trace_begin("WRITE");
#endif

	if( false == mIsOutCBStarted ){
#ifdef OPENSL_TRACE_DEBUG
		trace_end();
#endif
		return 0;
	}

	if( true == mNeedDriverRefresh ){
		NotifyXRun((jack_time_t)mCurOutCBTime*1000, (jack_time_t)((mCurOutCBTime - mPrevOutCBTime)*1000));
		refreshDriver();
		mNeedDriverRefresh = false;
		return 0;
	}

    jack_default_audio_sample_t* outputBuffer_1 = GetOutputBuffer(0);
    jack_default_audio_sample_t* outputBuffer_2 = GetOutputBuffer(1);

    if( outputBuffer_1 == NULL ) return -1;
    if( outputBuffer_2 == NULL ) return -1;

    if (mpOutBuf) {
        for (unsigned int i = 0, j = 0; i < fEngineControl->fBufferSize; i++) {
			FLOAT_TO_SHORT(*(outputBuffer_1 + i), *(mpOutBuf + j)); j++;
			FLOAT_TO_SHORT(*(outputBuffer_2 + i), *(mpOutBuf + j)); j++;
        }
    }

#ifdef ENABLE_JACK_LOGGER
	if((NULL != mJackLogger) && (mJackLogger->isStartedPcmDump())){
		mJackLogger->dumpWriteData((float*)outputBuffer_1, mFrameSize);
	}
#endif    

	mpOutQueue->EnQueue(mpOutBuf, mFrameSize * mOutChannelCnt);

	//jack_error("OpenSLDriver::Write() mpOutQueue[%d]", mpOutQueue->getItemCount());

	if( true == mIsBigBufferPlayback ){
		pthread_mutex_lock(&mOutMutex);
		pthread_cond_signal(&mOutCond);
		pthread_mutex_unlock(&mOutMutex);
	}

	CycleTakeBeginTime();

#ifdef OPENSL_TRACE_DEBUG
	trace_end();
#endif

    return 0;
}


int OpenSLDriver::Start(){

	pthread_mutex_lock(&mDriverControlMutex);
    jack_info("OpenSLDriver::Start");

	if ( true == mIsCapture ){
		if( SL_RESULT_SUCCESS != startAudioRecording()){
			jack_error(" OpenSLDriver::Start() startAudioRecording() failed");
			pthread_mutex_unlock(&mDriverControlMutex);
			return -1;
		}
	}

	if ( true == mIsPlayback ){
		if( SL_RESULT_SUCCESS != startBufferQueueAudioPlayer()){
			jack_error(" OpenSLDriver::Start() startBufferQueueAudioPlayer() failed");
			pthread_mutex_unlock(&mDriverControlMutex);
			return -1;
		}
	}

	JackDriver::Start();
	pthread_mutex_unlock(&mDriverControlMutex);
	return 0;
}


int OpenSLDriver::Stop(){

	pthread_mutex_lock(&mDriverControlMutex);
    jack_info("OpenSLDriver::Stop");
	JackDriver::Stop();

	if ( true == mIsPlayback ){
		if( SL_RESULT_SUCCESS != stopBufferQueueAudioPlayer()){
			jack_error(" OpenSLDriver::Stop() stopBufferQueueAudioPlayer() failed");
			pthread_mutex_unlock(&mDriverControlMutex);
			return -1;
		}
	}

	if ( true == mIsCapture ){
		if( SL_RESULT_SUCCESS != stopAudioRecording()){
			jack_error(" OpenSLDriver::Stop() stopAudioRecording() failed");
			pthread_mutex_unlock(&mDriverControlMutex);
			return -1;
		}
	}
	
	pthread_mutex_unlock(&mDriverControlMutex);
	return 0;
}




/////////////////////////// ++ OPENSL Functions Start ++ ////////////////////////////////////////

SLresult OpenSLDriver::createEngine(){

    SLresult result;

    result = slCreateEngine(&engineObject, 0, NULL, 0, NULL, NULL);
    if (SL_RESULT_SUCCESS != result){
		jack_error("OpenSLDriver::createEngine() slCreateEngine error [%d]", result);
		return result;
    }

    result = (*engineObject)->Realize(engineObject, SL_BOOLEAN_FALSE);
    if (SL_RESULT_SUCCESS != result){
		jack_error("OpenSLDriver::createEngine() engineObject Realize error [%d]", result);
		return result;
    }

    result = (*engineObject)->GetInterface(engineObject, SL_IID_ENGINE, &engineEngine);
    if (SL_RESULT_SUCCESS != result){
		jack_error("OpenSLDriver::createEngine() GetInterface error [%d]", result);
		return result;
    }

    const SLInterfaceID ids[1] = {SL_IID_VOLUME};
    const SLboolean req[1] = {SL_BOOLEAN_FALSE};
    result = (*engineEngine)->CreateOutputMix(engineEngine, &outputMixObject, 1, ids, req);
    if (SL_RESULT_SUCCESS != result){
		jack_error("OpenSLDriver::createEngine() CreateOutputMix error [%d]", result);
		return result;
    }

    result = (*outputMixObject)->Realize(outputMixObject, SL_BOOLEAN_FALSE);
    if (SL_RESULT_SUCCESS != result){
		jack_error("OpenSLDriver::createEngine() outputMixObject Realize error [%d]", result);
		return result;
    }

	// create buffers
	// for capture
	mpInBuf = (short *) malloc(sizeof(short) * mInBufSize);
	if( mpInBuf == NULL ){
		jack_error("OpenSLDriver::Open mpInBuf is NULL");
		return SL_RESULT_MEMORY_FAILURE;
	}
	memset(mpInBuf, 0, sizeof(short) * mInBufSize);
	
	mpInQueue = new HsCirQueue<short>(mInBufSize * 100);

	mpReadBuf = (short *) malloc(sizeof(short) * mInBufSize);
	if( mpReadBuf == NULL ){
		jack_error("OpenSLDriver::Open mpReadBuf is NULL");
		return SL_RESULT_MEMORY_FAILURE;
	}
	memset(mpReadBuf, 0, sizeof(short) * mInBufSize);

	// for playback
    mpPCM_OneFrame = (short*) malloc(sizeof(short) * mOutBufSize);
	if( mpPCM_OneFrame == NULL ){
		jack_error("OpenSLDriver::Open mpPCM_OneFrame is NULL");
		return SL_RESULT_MEMORY_FAILURE;
	}
    memset(mpPCM_OneFrame, 0, sizeof(short) * mOutBufSize);

	mpPCM_MutedOneFrame = (short*) malloc(sizeof(short) * mOutBufSize);
	if( mpPCM_MutedOneFrame == NULL ){
		jack_error("OpenSLDriver::Open mpPCM_MutedOneFrame is NULL");
		return SL_RESULT_MEMORY_FAILURE;
	}
    memset(mpPCM_MutedOneFrame, 0, sizeof(short) * mOutBufSize);

	mpOutQueue = new HsCirQueue<short>(mOutBufSize * 100);

	mpOutBuf = (short *) malloc(sizeof(short) * mOutBufSize);
	if( mpOutBuf == NULL ){
		jack_error("OpenSLDriver::Open mpOutBuf is NULL");
		return SL_RESULT_MEMORY_FAILURE;
	}
	memset(mpOutBuf, 0, sizeof(short) * mOutBufSize);

	mIsFirstRead = true;
	mBTFakePlayCnt = 0;

	mIsBigBufferPlayback = false;
	mBigBufferCheckCnt = 0;
	mIsBigBufferChecked = false;
	mBigBufferCBCnt = 0;

	return SL_RESULT_SUCCESS;
}

void OpenSLDriver::deleteEngine(){

    if (outputMixObject != NULL) {
        (*outputMixObject)->Destroy(outputMixObject);
        outputMixObject = NULL;
    }

    if (engineObject != NULL) {
        (*engineObject)->Destroy(engineObject);
        engineObject = NULL;
        engineEngine = NULL;
    }

	// delete buffers
	// for capture
	if(mpInQueue != NULL) {
		delete mpInQueue;
		mpInQueue = NULL;
	}

	if (mpReadBuf) {
		free(mpReadBuf);
		mpReadBuf = NULL;
	}

    if (mpInBuf) {
        free(mpInBuf);
        mpInBuf = NULL;
    }

	// for playback
	if(mpOutQueue != NULL) {
		delete mpOutQueue;
		mpOutQueue = NULL;
	}

	if(mpPCM_OneFrame != NULL){
		free(mpPCM_OneFrame);
		mpPCM_OneFrame = NULL;
	}

	if(mpPCM_MutedOneFrame != NULL){
		free(mpPCM_MutedOneFrame);
		mpPCM_MutedOneFrame = NULL;
	}

	if (mpOutBuf) {
		free(mpOutBuf);
		mpOutBuf = NULL;
	}
}


SLresult OpenSLDriver::createBufferQueueAudioPlayer(){

    SLresult result;
	SLuint32 channelMask = SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT;

	if( mOutChannelCnt == 1){
		channelMask = SL_SPEAKER_FRONT_CENTER;
	}

    if (bqPlayerObject != NULL) {
        deleteBufferQueueAudioPlayer();
    }

    SLDataLocator_AndroidSimpleBufferQueue loc_bufq = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2};

    SLDataFormat_PCM format_pcm = {SL_DATAFORMAT_PCM, mOutChannelCnt, SL_SAMPLINGRATE_48,
                                   SL_PCMSAMPLEFORMAT_FIXED_16, SL_PCMSAMPLEFORMAT_FIXED_16,
                                   channelMask, SL_BYTEORDER_LITTLEENDIAN};

    format_pcm.samplesPerSec = mSampleRate * 1000;       //sample rate in mili second

    SLDataSource audioSrc = {&loc_bufq, &format_pcm};

    SLDataLocator_OutputMix loc_outmix = {SL_DATALOCATOR_OUTPUTMIX, outputMixObject};

    SLDataSink audioSnk = {&loc_outmix, NULL};

    const SLInterfaceID ids[3] = {SL_IID_BUFFERQUEUE, SL_IID_VOLUME};
    const SLboolean req[3] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE,
            /*SL_BOOLEAN_TRUE,*/ };

    result = (*engineEngine)->CreateAudioPlayer(engineEngine, &bqPlayerObject, &audioSrc, &audioSnk, 2, ids, req);

    if (SL_RESULT_SUCCESS != result)return result;

    result = (*bqPlayerObject)->Realize(bqPlayerObject, SL_BOOLEAN_FALSE);
    if (SL_RESULT_SUCCESS != result)return result;

    result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_PLAY, &bqPlayerPlay);
    if (SL_RESULT_SUCCESS != result)return result;

    result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_BUFFERQUEUE, &bqPlayerBufferQueue);
    if (SL_RESULT_SUCCESS != result)return result;

    result = (*bqPlayerBufferQueue)->RegisterCallback(bqPlayerBufferQueue, bqPlayerCallback, this);
    if (SL_RESULT_SUCCESS != result)return result;

    result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_VOLUME, &bqPlayerVolume);
    if (SL_RESULT_SUCCESS != result)return result;

	return SL_RESULT_SUCCESS;
}

SLresult OpenSLDriver::startBufferQueueAudioPlayer(){

    SLresult result;

    if(mpOutQueue == NULL) return SL_RESULT_MEMORY_FAILURE;
    if(mpPCM_OneFrame == NULL) return SL_RESULT_MEMORY_FAILURE;

	mPrevOutCBTime = getMilliSecond();

    result = (*bqPlayerPlay)->SetPlayState(bqPlayerPlay, SL_PLAYSTATE_PLAYING);
    if (SL_RESULT_SUCCESS != result)return result;

	mIsOutStarted = false;
	mIsOutCBStarted = false;
	mPrevOutCBTime = 0;
	mUnderRunCnt = 0;
	mOverRunCnt = 0;

	result = (*bqPlayerBufferQueue)->Enqueue(bqPlayerBufferQueue, mpPCM_OneFrame, mOutBufSize * sizeof(short));

	return result;

}


SLresult OpenSLDriver::stopBufferQueueAudioPlayer(){

    SLresult result;

    result = (*bqPlayerPlay)->SetPlayState(bqPlayerPlay, SL_PLAYSTATE_STOPPED);
    if (SL_RESULT_SUCCESS != result)return result;

	result = (*bqPlayerBufferQueue)->Clear(bqPlayerBufferQueue);
	if (SL_RESULT_SUCCESS != result)return result; 

	return SL_RESULT_SUCCESS;
}


void OpenSLDriver::bqPlayerCallback(SLAndroidSimpleBufferQueueItf bq, void *context){

#ifdef OPENSL_TRACE_DEBUG
	trace_begin("OUT_CB");
#endif

    SLresult result;
	bool processRet = true;

    OpenSLDriver *driver = (OpenSLDriver*) context;

	driver->mCurOutCBTime = driver->getMilliSecond();

    if((driver->mpOutQueue == NULL)
		||(driver->mpPCM_OneFrame == NULL)
		||(driver->mpPCM_MutedOneFrame == NULL )){
#ifdef OPENSL_TRACE_DEBUG
		trace_end();
#endif
		return;
    }

	//jack_error("OpenSLDriver::bqPlayerCallback mpOutQueue Cnt[%d] curCBTime[%lld ms] diff[%d]", 
	//	driver->mpOutQueue->getItemCount(), driver->mCurOutCBTime, driver->mCurOutCBTime - driver->mPrevOutCBTime);

	if( false == driver->mIsBigBufferChecked ){
		driver->checkBigBufferPlayback(driver->mCurOutCBTime - driver->mPrevOutCBTime);
	}

	driver->mIsOutCBStarted = true;

	if( false == driver->mIsBigBufferPlayback ){
		processRet = processPlayback(context);
	} else{
		processRet = processBigBufferPlayback(context);
	}

	driver->mPrevOutCBTime = driver->mCurOutCBTime;

	if (false == processRet){  // Skip enqueue
#ifdef OPENSL_TRACE_DEBUG
		trace_end();
#endif
		return;
	}

    result = (*(driver->bqPlayerBufferQueue))->Enqueue(driver->bqPlayerBufferQueue, driver->mpPCM_OneFrame, 
		driver->mOutBufSize * sizeof(short));

	if( result != SL_RESULT_SUCCESS) {
		jack_error("OpenSLDriver::bqPlayerCallback Enqueue failed[%d]", result);
	}

#ifdef OPENSL_TRACE_DEBUG
	trace_end();
#endif

}


bool OpenSLDriver::processPlayback(void *context){
	
    OpenSLDriver *driver = (OpenSLDriver*) context;

	if( !driver->mInChannelCnt ){
		// To playback only mode.
		pthread_mutex_lock(&(driver->mInMutex));
		driver->mpInQueue->EnQueue(driver->mpPCM_MutedOneFrame, driver->mInBufSize);
		pthread_cond_signal(&(driver->mInCond));
		pthread_mutex_unlock(&(driver->mInMutex));
	}
	
	if(false == driver->mIsInCBStarted && true == driver->mIsCapture){
		return true;
	}

	if( (driver->mOutJitterCount) <= (driver->mpOutQueue->getItemCount()) && (driver->mIsOutStarted == false) ){
		// start state
		while(driver->mpOutQueue->getItemCount() > driver->mOutJitterCount ){
			driver->mpOutQueue->DeQueue(driver->mpPCM_OneFrame, driver->mOutBufSize);
		}
		
		driver->mpOutQueue->DeQueue(driver->mpPCM_OneFrame, driver->mOutBufSize);
		driver->mIsOutStarted = true;
	} else if(driver->mIsOutStarted == true){
		// playing state
		// ++ check callback delay ++
		if( driver->mPrevOutCBTime < driver->mCurOutCBTime ){
			unsigned int diff = driver->mCurOutCBTime - driver->mPrevOutCBTime;
			if( diff > (driver->mOutBufLatency * XRUN_CNT_LIMIT)){
				jack_error("OpenSLDriver::bqPlayerCallback can't receive bqPlayerCallback during [%d]ms, prevTime[%lld], curTime[%lld]", 
					diff, driver->mPrevOutCBTime, driver->mCurOutCBTime);
				driver->mNeedDriverRefresh = true;
				return false;
			}
		} else if(driver->mPrevOutCBTime > driver->mCurOutCBTime){
			if( driver->mCurOutCBTime > ((unsigned long long)driver->mOutBufLatency * XRUN_CNT_LIMIT)){
				jack_error("OpenSLDriver::bqPlayerCallback can't receive bqPlayerCallback during [%d]ms", driver->mCurOutCBTime);
				driver->mNeedDriverRefresh = true;
				return false;
			}
		}
		// -- check callback delay --


		// ++ check xrun ++
		if (driver->mpOutQueue->getItemCount() <= 0){
			driver->mUnderRunCnt ++;
			if(driver->mUnderRunCnt >= XRUN_CNT_LIMIT){
				jack_error("OpenSLDriver::bqPlayerCallback uder_run case  mpOutQueue[%d].\n It mean app process is blocked or In CB is not working.", 
					driver->mpOutQueue->getItemCount());
				driver->mNeedDriverRefresh = true;
				return false;
			}
			// enqueue data one more to increase queue count.
			driver->mpOutQueue->EnQueue(driver->mpPCM_MutedOneFrame,driver->mOutBufSize);			
		}else if (driver->mpOutQueue->getItemCount() >= (driver->mOutJitterCount*2)){
			driver->mOverRunCnt ++;
			if(driver->mOverRunCnt >= XRUN_CNT_LIMIT){
				jack_error("OpenSLDriver::bqPlayerCallback over_run case  mpOutQueue[%d].\n It mean cputure callback came more than playback callback", 
					driver->mpOutQueue->getItemCount());
				driver->mNeedDriverRefresh = true;
				return false;
			}
			// dequeue data one more to reduce queue count.
			driver->mpOutQueue->DeQueue(driver->mpPCM_OneFrame, driver->mOutBufSize);
		}else{
			driver->mUnderRunCnt = 0;
			driver->mOverRunCnt = 0;
		}
		// -- check xrun --

		// get pcm data from queue in normal case.
		driver->mpOutQueue->DeQueue(driver->mpPCM_OneFrame, driver->mOutBufSize);
	}
	return true;

}



bool OpenSLDriver::processBigBufferPlayback(void *context){

    OpenSLDriver *driver = (OpenSLDriver*) context;

	if( !driver->mInChannelCnt ){
		// To playback only mode.
		pthread_mutex_lock(&(driver->mInMutex));
		driver->mpInQueue->EnQueue(driver->mpPCM_MutedOneFrame, driver->mInBufSize);
		pthread_cond_signal(&(driver->mInCond));
		pthread_mutex_unlock(&(driver->mInMutex));
	}

	if(false == driver->mIsInCBStarted && true == driver->mIsCapture){
		return true;
	}

	if( driver->mpOutQueue->getItemCount() < 0 ){
		driver->mpOutQueue->EnQueue(driver->mpPCM_MutedOneFrame, driver->mOutBufSize);
	} else {
		while(driver->mpOutQueue->getItemCount() != 0 ){
			driver->mpOutQueue->DeQueue(driver->mpPCM_OneFrame, driver->mOutBufSize);
		}
	}

	pthread_mutex_lock(&(driver->mOutMutex));
	if( driver->mpOutQueue->getItemCount() <= 0 ){
		if( 0 != driver->timedWait(&(driver->mOutCond), &(driver->mOutMutex), driver->mOutBufLatency*30) ){
			jack_error("OpenSLDriver::processBigBufferPlayback timedWait error");
		}
	}
	pthread_mutex_unlock(&(driver->mOutMutex));


	// Enqueue half pcm data to reduce bt latency. we don't know why this action reduce bt playback latency.
	if( driver->mBTFakePlayCnt < 200 ){
		(*(driver->bqPlayerBufferQueue))->Enqueue(driver->bqPlayerBufferQueue, driver->mpPCM_MutedOneFrame, 
			driver->mOutBufSize * sizeof(short) / 2);
		driver->mBTFakePlayCnt ++;
		return false;
	}

	// get pcm data from queue in normal case.
	driver->mpOutQueue->DeQueue(driver->mpPCM_OneFrame, driver->mOutBufSize);

	return true;
}




void OpenSLDriver::deleteBufferQueueAudioPlayer(){

    if (bqPlayerObject != NULL) {
        (*bqPlayerObject)->Destroy(bqPlayerObject);
        bqPlayerObject = NULL;
        bqPlayerPlay = NULL;
        bqPlayerBufferQueue = NULL;
        bqPlayerVolume = NULL;
    }

	mIsOutStarted = false;
	mIsOutCBStarted = false;
	mPrevOutCBTime = 0;
}


void OpenSLDriver::setVolume(float volume){

	if( NULL == bqPlayerVolume )return;
	
	SLmillibel level = 2000 * log10(volume);
	(*bqPlayerVolume)->SetVolumeLevel(bqPlayerVolume, level);
}


SLresult OpenSLDriver::createAudioRecorder() {

    SLresult result;
	SLuint32 channelMask = SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT;

	if( mDriverInChannelCnt == 1){
		channelMask = SL_SPEAKER_FRONT_CENTER;
	}

    if (recorderObject != NULL) {
        deleteAudioRecorder();
    }

    SLDataLocator_IODevice loc_dev = {SL_DATALOCATOR_IODEVICE, SL_IODEVICE_AUDIOINPUT,
                                      SL_DEFAULTDEVICEID_AUDIOINPUT, NULL};
    SLDataSource audioSrc = {&loc_dev, NULL};

    SLDataLocator_AndroidSimpleBufferQueue loc_bq = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2};
    SLDataFormat_PCM format_pcm = {SL_DATAFORMAT_PCM, mDriverInChannelCnt, SL_SAMPLINGRATE_48,
                                   SL_PCMSAMPLEFORMAT_FIXED_16, SL_PCMSAMPLEFORMAT_FIXED_16,
                                   channelMask, SL_BYTEORDER_LITTLEENDIAN};

    format_pcm.samplesPerSec = mSampleRate * 1000;

    SLDataSink audioSnk = {&loc_bq, &format_pcm};

    const SLInterfaceID id[2] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_ANDROIDCONFIGURATION};
    const SLboolean req[2] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_FALSE};
    result = (*engineEngine)->CreateAudioRecorder(engineEngine, &recorderObject, &audioSrc, &audioSnk, 2, id, req);
    if (SL_RESULT_SUCCESS != result)return result;

    SLAndroidConfigurationItf inputConfiguration;
    if ((result = (*recorderObject)->GetInterface(recorderObject, SL_IID_ANDROIDCONFIGURATION, &inputConfiguration)) == SL_RESULT_SUCCESS) {
        SLuint32 presetValue = SL_ANDROID_RECORDING_PRESET_VOICE_RECOGNITION;
        (*inputConfiguration)->SetConfiguration(inputConfiguration, SL_ANDROID_KEY_RECORDING_PRESET, &presetValue, sizeof(SLuint32));
    }else{
        jack_error("SL_IID_ANDROIDCONFIGURATION interface failed");
    };

    result = (*recorderObject)->Realize(recorderObject, SL_BOOLEAN_FALSE);
    if (SL_RESULT_SUCCESS != result)return result;

    result = (*recorderObject)->GetInterface(recorderObject, SL_IID_RECORD, &recorderRecord);
    if (SL_RESULT_SUCCESS != result)return result;

    result = (*recorderObject)->GetInterface(recorderObject, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &recorderBufferQueue);
    if (SL_RESULT_SUCCESS != result)return result;

    result = (*recorderBufferQueue)->RegisterCallback(recorderBufferQueue, bqRecorderCallback, this);
    if (SL_RESULT_SUCCESS != result)return result;
	
    return SL_RESULT_SUCCESS;
}


SLresult OpenSLDriver::startAudioRecording(){

    SLresult result;

	result = stopAudioRecording();

	if( SL_RESULT_SUCCESS != result){
		return result;
	}
	
	mPrevInCBTime = getMilliSecond();

	mIsInCBStarted = false;
	mPrevInCBTime = 0;

    result = (*recorderBufferQueue)->Enqueue(recorderBufferQueue, mpInBuf, mFrameSize * mDriverInChannelCnt * sizeof(short));
    if (SL_RESULT_SUCCESS != result)return result; 

    result = (*recorderRecord)->SetRecordState(recorderRecord, SL_RECORDSTATE_RECORDING);
    if (SL_RESULT_SUCCESS != result)return result;

	return SL_RESULT_SUCCESS;
}


SLresult OpenSLDriver::stopAudioRecording(){

    SLresult result;

    result = (*recorderRecord)->SetRecordState(recorderRecord, SL_RECORDSTATE_STOPPED);
    if (SL_RESULT_SUCCESS != result)return result; 

	result = (*recorderBufferQueue)->Clear(recorderBufferQueue);
	if (SL_RESULT_SUCCESS != result)return result; 

	return SL_RESULT_SUCCESS;
}



void OpenSLDriver::deleteAudioRecorder(){

    if (recorderObject != NULL) {
        (*recorderObject)->Destroy(recorderObject);
        recorderObject = NULL;
        recorderRecord = NULL;
        recorderBufferQueue = NULL;
    }
	
	mIsInCBStarted = false;

}


void OpenSLDriver::bqRecorderCallback(SLAndroidSimpleBufferQueueItf bq, void *context)
{

#ifdef OPENSL_TRACE_DEBUG
	trace_begin("IN_CB");
#endif
    SLresult result;
    OpenSLDriver *driver = (OpenSLDriver*) context;
	driver->mIsInCBStarted = true;
	driver->mCurInCBTime = driver->getMilliSecond();
		
    if( driver->mpInBuf == NULL ) return;
	if( driver->mpInQueue == NULL ) return;

	pthread_mutex_lock(&(driver->mInMutex));
	driver->mpInQueue->EnQueue(driver->mpInBuf, driver->mFrameSize * driver->mDriverInChannelCnt);

	//jack_error("OpenSLDriver::bqRecorderCallback mpInQueue[%d] diff[%d]", driver->mpInQueue->getItemCount(), driver->mCurInCBTime - driver->mPrevInCBTime);
	
    result = (*(driver->recorderBufferQueue))->Enqueue(driver->recorderBufferQueue, driver->mpInBuf, driver->mFrameSize * driver->mDriverInChannelCnt * sizeof(short));
	if( result != SL_RESULT_SUCCESS) {
		jack_error("OpenSLDriver::bqRecorderCallback Enqueue failed[%d]", result);
	}

#ifdef OPENSL_TRACE_DEBUG
	trace_end();
#endif

	pthread_cond_signal(&(driver->mInCond));
	pthread_mutex_unlock(&(driver->mInMutex));
	driver->mPrevInCBTime = driver->mCurInCBTime;
}


int OpenSLDriver::refreshDriver(){

	pthread_mutex_lock(&mDriverControlMutex);
	jack_info("OpenSLDriver::refreshDriver start");

	if( NULL == engineEngine ){
		jack_info("OpenSLDriver::refreshDriver engineEngine is NULL. It's closed status, so skip refresh driver");
		pthread_mutex_unlock(&mDriverControlMutex);
		return 0;
	}

	SLresult result = SL_RESULT_SUCCESS;

	deleteAudioRecorder();
	deleteBufferQueueAudioPlayer();
	deleteEngine();

	// Create OpenSL Engine
	result = createEngine();

	if( result != SL_RESULT_SUCCESS) {
	    jack_error("OpenSLDriver::refreshDriver createEngine failed[%d]", result);
       	pthread_mutex_unlock(&mDriverControlMutex);
		return -1;
	}

	// In Create
    if (mIsCapture) {
		result = createAudioRecorder();
		if( result != SL_RESULT_SUCCESS) {
		    jack_error("OpenSLDriver::refreshDriver createAudioRecorder failed[%d]", result);
			pthread_mutex_unlock(&mDriverControlMutex);
			return -1;
		}
    }

	// In Start
	if(mIsCapture){
		result = startAudioRecording();
		if( result != SL_RESULT_SUCCESS) {
		    jack_error("OpenSLDriver::refreshDriver startAudioRecording failed[%d]", result);
			pthread_mutex_unlock(&mDriverControlMutex);
			return -1;
		}
	}

	// Out Create
    if (mIsPlayback) {
		result = createBufferQueueAudioPlayer();
		if( result != SL_RESULT_SUCCESS) {
		    jack_error("OpenSLDriver::refreshDriver createBufferQueueAudioPlayer failed[%d]", result);
			pthread_mutex_unlock(&mDriverControlMutex);
			return -1;
		}
    }

	// Out Start
	if(mIsPlayback){
		result = startBufferQueueAudioPlayer();
		if( result != SL_RESULT_SUCCESS) {
		    jack_error("OpenSLDriver::refreshDriver startBufferQueueAudioPlayer failed[%d]", result);
			pthread_mutex_unlock(&mDriverControlMutex);
			return -1;
		}
	}

	jack_info("OpenSLDriver::refreshDriver end");
	pthread_mutex_unlock(&mDriverControlMutex);
	return 0;
}


unsigned long long OpenSLDriver::getMilliSecond(){

	struct timespec res;
    clock_gettime(CLOCK_MONOTONIC, &res);
	return ((unsigned long long)res.tv_sec * 1000) + ((unsigned long long)res.tv_nsec / 1000000);
}


int OpenSLDriver::timedWait(pthread_cond_t *pCond, pthread_mutex_t *pMutex, int milliSecTimeWaiting){
	
	timespec time;
	struct timeval now;
	int res;
	
	gettimeofday(&now, 0);
	unsigned int next_date_usec = now.tv_usec + (milliSecTimeWaiting * 1000);
	time.tv_sec = now.tv_sec + (next_date_usec / 1000000);
	time.tv_nsec = (next_date_usec % 1000000) * 1000;
	
	res = pthread_cond_timedwait(pCond, pMutex, &time);
	if (res != 0) {
		jack_error("OpenSLDriver::timedWait - pthread_cond_timedwait out err[%s]", strerror(res));
	}

	return res;
}


void OpenSLDriver::checkBigBufferPlayback(unsigned int timeDiff){

	//jack_info("OpenSLDriver::checkBigBufferPlayback - diff[%d], cnt[%d]", timeDiff, mBigBufferCheckCnt);

	if(mBigBufferCheckCnt > 10 ){
		// check first 10 times only
		mIsBigBufferChecked = true;
		jack_info("OpenSLDriver::checkBigBufferPlayback - set Normal Buffer Playback");
		return;
	}

	if( true == mIsOutCBStarted ){
		if(timeDiff <= 1){
			mBigBufferCBCnt++;
		}else{
			mBigBufferCBCnt = 0;
		}
	}

	if(mBigBufferCBCnt > 3 ){
		mIsBigBufferPlayback = true;
		mIsBigBufferChecked = true;
		mBigBufferCheckCnt = 10;
		jack_info("OpenSLDriver::checkBigBufferPlayback - set Big Buffer Playback");
	}

	mBigBufferCheckCnt++;
}



/////////////////////////// -- OPENSL Functions End -- ////////////////////////////////////////

} // end of namespace



#ifdef __cplusplus
extern "C"
{
#endif


    SERVER_EXPORT jack_driver_desc_t * driver_get_descriptor () {
        jack_driver_desc_t * desc;
        jack_driver_desc_filler_t filler;
        jack_driver_param_value_t value;

        desc = jack_driver_descriptor_construct("opensles", JackDriverMaster, "Timer based backend", &filler);

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

		Jack::OpenSLDriver* opensl_driver = new Jack::OpenSLDriver("system", "opensles_pcm", engine, table);
		opensl_driver->SetJitterCnt(user_nperiods);
	
		Jack::JackDriverClientInterface* driver = new Jack::JackThreadedDriver(opensl_driver);
		if (driver->Open(buffer_size, sample_rate, capture_ports? 1 : 0, playback_ports? 1 : 0, capture_ports, playback_ports, monitor, "opensles", "opensles", 0, 0) == 0) {
			return driver;
		} else {
			delete driver;
			return NULL;
		}
    }

#ifdef __cplusplus
}
#endif


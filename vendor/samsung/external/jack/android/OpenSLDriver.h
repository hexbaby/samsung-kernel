//  License:
//
//  This source code is provided as is, without warranty.
//  You may copy and distribute verbatim copies of this document.
//  You may modify and use this source code to create binary code for your own purposes, free or commercial.
//


#ifndef __OPEN_SL_DRIVER_H__
#define __OPEN_SL_DRIVER_H__

#include "JackAudioDriver.h"
#include <stdlib.h>
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <pthread.h>
#include "HsCirQueue.h"

namespace Jack
{

class OpenSLDriver : public JackAudioDriver
{
    public:

        OpenSLDriver(const char* name, const char* alias, JackLockedEngine* engine, JackSynchro* table);
        virtual ~OpenSLDriver();

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
		
		void SetJitterCnt(int cnt){ mOutJitterCount = cnt; };

	private:

	    SLresult createEngine();
	    void deleteEngine();

	    SLresult createBufferQueueAudioPlayer();
	    SLresult startBufferQueueAudioPlayer();
	    SLresult stopBufferQueueAudioPlayer();
	    void deleteBufferQueueAudioPlayer();
	    static void bqPlayerCallback(SLAndroidSimpleBufferQueueItf bq, void *context);
	    static bool processPlayback(void *context);
	    static bool processBigBufferPlayback(void *context);
		
		void setVolume(float volume);
		
		SLresult createAudioRecorder();
		SLresult startAudioRecording();
		SLresult stopAudioRecording();
		void deleteAudioRecorder();
		static void bqRecorderCallback(SLAndroidSimpleBufferQueueItf bq, void *context);

		int refreshDriver();
		unsigned long long getMilliSecond();
		int timedWait(pthread_cond_t *pCond, pthread_mutex_t *pMutex, int milliSecTimeWaiting);
		void checkBigBufferPlayback(unsigned int timeDiff);

		SLObjectItf engineObject;
		SLEngineItf engineEngine;
		SLObjectItf outputMixObject;

		SLObjectItf bqPlayerObject;
		SLPlayItf bqPlayerPlay;
		SLAndroidSimpleBufferQueueItf bqPlayerBufferQueue;
		SLVolumeItf bqPlayerVolume;

		SLObjectItf recorderObject;
		SLRecordItf recorderRecord;
		SLAndroidSimpleBufferQueueItf recorderBufferQueue;

		bool mIsPlayback;
		bool mIsCapture;
		unsigned int mSampleRate;
		unsigned int mFrameSize;
		unsigned int mOutChannelCnt;
		unsigned int mInChannelCnt;
		unsigned int mDriverInChannelCnt;
		unsigned int mOutBufSize;
		unsigned int mInBufSize;
		
		bool mIsOutStarted;
		bool mIsOutCBStarted;
		bool mIsInCBStarted;
		bool mIsFirstRead;
		int mOutJitterCount;
		int mOutBufLatency;
	
		unsigned long long mPrevOutCBTime;
		unsigned long long mCurOutCBTime;
		unsigned long long mPrevInCBTime;
		unsigned long long mCurInCBTime;

		bool mNeedDriverRefresh;
		int mUnderRunCnt;
		int mOverRunCnt;

		short *mpPCM_OneFrame;
		short *mpPCM_MutedOneFrame;
	    HsCirQueue<short> *mpOutQueue;
	    HsCirQueue<short> *mpInQueue;
        short *mpInBuf;
		short *mpOutBuf;
		short *mpReadBuf;

		bool mIsBigBufferPlayback;
		int mBigBufferCheckCnt;
		int mBigBufferCBCnt;
		bool mIsBigBufferChecked;
		
		int mBTFakePlayCnt;
		
		pthread_mutex_t mDriverControlMutex;
		pthread_mutex_t mInMutex;
		pthread_cond_t	mInCond;
		pthread_mutex_t mOutMutex;
		pthread_cond_t	mOutCond;

};

} // end of namespace

#endif


//
// Created by haksung.lyou on 2016-08-16.
//
//  License:
//
//  This source code is provided as is, without warranty.
//  You may copy and distribute verbatim copies of this document.
//  You may modify and use this source code to create binary code for your own purposes, free or commercial.
//


#ifndef HAKSAUDIOTEST_HSCIRQUEUE_H
#define HAKSAUDIOTEST_HSCIRQUEUE_H

enum HsCirQueueResult{
    HsCirQ_Success = 0,
    HsCirQ_OverWrited = -1
};

template <typename T>
class HsCirQueue {

public:
    HsCirQueue(int bufSize){
        mFirstPos = 0;
        mLastPos = 0;
        mBufSize = bufSize;
        mpData = new T[mBufSize];
        memset(mpData, 0, sizeof(T) * mBufSize);
        mRemainedDataSize = 0;
		mItemCnt = 0;

		mHsCirQueueMutex = PTHREAD_MUTEX_INITIALIZER;
		pthread_mutex_init(&mHsCirQueueMutex, (pthread_mutexattr_t*) NULL);
    };

    virtual ~HsCirQueue(){
		pthread_mutex_destroy(&mHsCirQueueMutex);
        delete[] mpData;
    };

    void EnQueue(T* data, int size);
    void DeQueue(T* data, int size);
    long long getRemainedDataSize(){ return mRemainedDataSize; }
	int getItemCount(){ return mItemCnt; }

private:
    int mFirstPos;
    int mLastPos;
    int mBufSize;
	int mItemCnt;
    T *mpData;

    long long mRemainedDataSize;
	pthread_mutex_t mHsCirQueueMutex;
};

template <typename T>
void HsCirQueue<T>::EnQueue(T* data, int size) {

	pthread_mutex_lock(&mHsCirQueueMutex);

    mRemainedDataSize += size;

    int remainedSize = mBufSize - mLastPos;
    int diff = remainedSize - size;
	mItemCnt++;

    if(diff > 0){
        memcpy(mpData + mLastPos, data, sizeof(T) * size);
        mLastPos += size;
    } else if ( diff == 0 ){
        memcpy(mpData + mLastPos, data, sizeof(T) * size);
        mLastPos = 0;
    } else if( diff < 0 ){
        int endRemain = size - remainedSize;
        memcpy(mpData + mLastPos, data, sizeof(T) * remainedSize);
        memcpy(mpData, data + remainedSize, sizeof(T) * endRemain);
        mLastPos = endRemain;
    }
	
	pthread_mutex_unlock(&mHsCirQueueMutex);
}

template <typename T>
void HsCirQueue<T>::DeQueue(T* data, int size) {

	pthread_mutex_lock(&mHsCirQueueMutex);

    mRemainedDataSize -= size;
	mItemCnt--;

    int remainedSize = mBufSize - mFirstPos;
    int diff = remainedSize - size;

    if(diff > 0){
        memcpy(data, mpData + mFirstPos, sizeof(T) * size);
        mFirstPos += size;
    } else if ( diff == 0 ){
        memcpy(data, mpData + mFirstPos, sizeof(T) * size);
        mFirstPos = 0;
    } else if( diff < 0 ){
        int endRemain = size - remainedSize;
        memcpy(data, mpData + mFirstPos, sizeof(T) * remainedSize);
        memcpy(data + remainedSize , mpData, sizeof(T) * endRemain);
        mFirstPos = endRemain;
    }
	
	pthread_mutex_unlock(&mHsCirQueueMutex);
}

#endif //HAKSAUDIOTEST_HSCIRQUEUE_H


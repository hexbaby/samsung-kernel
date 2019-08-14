#ifndef __JackDebugger__
#define __JackDebugger__

#include <stdio.h>
#include "types.h"

#define ATRACE_MESSAGE_LEN 256

#ifdef __cplusplus
extern "C"
{
#endif

void trace_init();
SERVER_EXPORT void trace_begin(const char *name);
SERVER_EXPORT void trace_end();
SERVER_EXPORT void trace_counter(const char *name, const int value);
SERVER_EXPORT void trace_async_begin(const char *name, const int32_t cookie);
SERVER_EXPORT void trace_async_end(const char *name, const int32_t cookie);
SERVER_EXPORT void setSysTrace(char *msg, unsigned long value);

void initAllTime();
void initTime(int index);
bool isEmptyTable();
bool isFullTable();
int findItem(char *key);
void updateItem(int index, char *key, jack_time_t start, jack_time_t end);
void putItem(char *key, jack_time_t start, jack_time_t end);

SERVER_EXPORT void setStartTime(char *key);
SERVER_EXPORT void setEndTime(char *key);
SERVER_EXPORT void printDiffTime(char *key, char *msg);

#ifdef __cplusplus
}
#endif

#endif

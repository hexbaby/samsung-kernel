#include <cutils/trace.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>

#include "JackError.h"
#include "JackDebugger.h"
#include "JackTime.h"

#define jack_get_microseconds GetMicroSeconds

#define TIME_MAX 10
#define KEY_MAX 30
#define INVALID_VALUE -1
#define EMPTY_STRING "empty"

int     atrace_marker_fd = INVALID_VALUE;

typedef struct {
	char key[KEY_MAX];
	jack_time_t start_test_time;
	jack_time_t finish_test_time;
} DEBUG_TIME;

DEBUG_TIME time_table[TIME_MAX];

/**
  *  @Usage : 1. Add #include "JackDebugger.h" to profiling class
  *          2. Call function for each use.
  */

void trace_init()
{
   atrace_marker_fd = open("/sys/kernel/debug/tracing/trace_marker", O_WRONLY);
   if (atrace_marker_fd == INVALID_VALUE)   {
      jack_error ("Fail to open trace_marker");
   }
}

SERVER_EXPORT void trace_begin(const char *name)
{
   if (atrace_marker_fd == INVALID_VALUE) {
      trace_init();
   }

   char buf[ATRACE_MESSAGE_LEN];
   int len = snprintf(buf, ATRACE_MESSAGE_LEN, "B|%d|%s", getpid(), name);

   if (atrace_marker_fd != INVALID_VALUE) {
      write(atrace_marker_fd, buf, len);
   }
}

SERVER_EXPORT void trace_end()
{
   char c = 'E';
   if (atrace_marker_fd != INVALID_VALUE) {
      write(atrace_marker_fd, &c, 1);
   }
}

/**
  *  @Exp : It can use between trace_begin and trace_end
  */
SERVER_EXPORT void trace_counter(const char *name, const int value)
{
   char buf[ATRACE_MESSAGE_LEN];
   int len = snprintf(buf, ATRACE_MESSAGE_LEN, "C|%d|%s|%i", getpid(), name, value);
   if (atrace_marker_fd != INVALID_VALUE) {
      write(atrace_marker_fd, buf, len);
   }
}

SERVER_EXPORT void trace_async_begin(const char *name, const int32_t cookie)
{
   char buf[ATRACE_MESSAGE_LEN];
   int len = snprintf(buf, ATRACE_MESSAGE_LEN, "S|%d|%s|%i", getpid(), name, cookie);
   if (atrace_marker_fd != INVALID_VALUE) {
      write(atrace_marker_fd, buf, len);
   }
}

SERVER_EXPORT void trace_async_end(const char *name, const int32_t cookie)
{
   char buf[ATRACE_MESSAGE_LEN];
   int len = snprintf(buf, ATRACE_MESSAGE_LEN, "F|%d|%s|%i", getpid(), name, cookie);
   if (atrace_marker_fd != INVALID_VALUE) {
      write(atrace_marker_fd, buf, len);
   }
}

/**
  *  @Exp : It can be used independently without trace_begin and trace_end.
  */
SERVER_EXPORT void setSysTrace(char *msg, unsigned long value)
{
   char buf[ATRACE_MESSAGE_LEN];
   snprintf(buf, ATRACE_MESSAGE_LEN, msg, value);

   trace_begin(buf);
   trace_end();
}


/**
  * @ To Do List - add to check diff time logic
 **/
void initAllTime() {
	int i = 0;

	while( i < TIME_MAX ) {
		strcpy(time_table[i].key , EMPTY_STRING);
		time_table[i].start_test_time = 0;
		time_table[i].finish_test_time = 0;
		i++;
	}
}

void initTime(int index) {
	strcpy(time_table[index].key , EMPTY_STRING);
	time_table[index].start_test_time = 0;
	time_table[index].finish_test_time = 0;
}


bool isEmptyTable() {
	if (strcmp(time_table[0].key, EMPTY_STRING))
		return false;
	return true;
}

bool isFullTable() {
	int i = 0;
	while( strcmp(time_table[i].key, EMPTY_STRING) ) {
		if ( i == TIME_MAX - 1)
			return true;
		i++;
	}

	return false;
}

int findItem(char *key) {
	int i = 0;

	if (key == NULL || isEmptyTable()) {
		return INVALID_VALUE;
	}

	while( i < TIME_MAX ) {
		if (!strcmp(key, time_table[i].key)) {
			return i;
		}
		i++;
	}
	return INVALID_VALUE;
}

void updateItem(int index, char *key, jack_time_t start, jack_time_t end) {
	strcpy(time_table[index].key, key);
	if (start > 0)
		time_table[index].start_test_time = start;
	if (end > 0)
		time_table[index].finish_test_time = end;
}

void putItem(char *key, jack_time_t start, jack_time_t end) {
	if (key == NULL) {
		return;
	}

	if (isEmptyTable() || isFullTable()) {
		initAllTime();
		updateItem(0, key, start, end);
	} 
}

SERVER_EXPORT void setStartTime(char *key)
{
	int index = INVALID_VALUE;
	jack_time_t start_time = jack_get_microseconds();

	if ( (index = findItem(key)) == INVALID_VALUE ) {
		putItem(key, start_time, 0);
	} else {
		time_table[index].start_test_time = start_time;
	}
}

SERVER_EXPORT void setEndTime(char *key)
{
	int index = INVALID_VALUE;
	jack_time_t end_time = jack_get_microseconds();

	if ( (index = findItem(key)) == INVALID_VALUE ) {
		putItem(key, 0, end_time);
	} else {
		time_table[index].finish_test_time = end_time;
	}
}

SERVER_EXPORT void printDiffTime(char *key, char *msg)
{
	jack_time_t start_time = 0;
	jack_time_t end_time = 0;
	jack_time_t diff_time = 0;
	jack_time_t ms_time = 0;
	jack_time_t point_number_time = 0;
	int index = INVALID_VALUE;

	if ( (index = findItem(key)) != INVALID_VALUE ) {
		start_time = time_table[index].start_test_time;
		end_time = time_table[index].finish_test_time;

		if (start_time <= 0 || end_time <= 0 )
			return;

		if (start_time > end_time) {
			diff_time = start_time - end_time;
			jack_error ("start > end");
		} else {
			diff_time = end_time - start_time;
		}
		ms_time = diff_time / 1000;
		point_number_time = diff_time % 1000;

		if (msg == NULL) {
			jack_error ("diff time = [ %lld.%lld ms ]", ms_time, point_number_time);
		} else {
			jack_error ("%s = [ %lld.%lld ms ]", msg, ms_time, point_number_time);
		}

		time_table[index].start_test_time = 0;
		time_table[index].finish_test_time = 0;
	//	initTime(index);
	}
}

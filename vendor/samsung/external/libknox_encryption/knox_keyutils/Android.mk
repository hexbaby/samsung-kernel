LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE:= libknox_keyutils

LOCAL_SRC_FILES := \
	knox_keyutils.c

LOCAL_C_INCLUDES := \

LOCAL_CFLAGS := -O2 -fomit-frame-pointer

LOCAL_MODULE_TAGS := optional
LOCAL_SHARED_LIBRARIES := \

LOCAL_PRELINK_MODULE := false

include $(BUILD_SHARED_LIBRARY)

LOCAL_PATH := $(call my-dir)

CORE_DIR := $(LOCAL_PATH)/..
include $(CORE_DIR)/Makefile.common

include $(CLEAR_VARS)
LOCAL_MODULE    := retro
LOCAL_SRC_FILES := $(SOURCES_C) $(SOURCES_CXX)
LOCAL_C_INCLUDES := $(patsubst -I%,%,$(INCFLAGS))
LOCAL_CFLAGS    := -D__LIBRETRO__ -DHAVE_ZLIB -std=gnu11 -Wno-unused-function
LOCAL_CPPFLAGS  := -D__LIBRETRO__ -DHAVE_ZLIB -std=c++14
LOCAL_LDLIBS    := -lz -lm
LOCAL_LDFLAGS   := -Wl,-version-script=$(CORE_DIR)/src/libretro/link.T
include $(BUILD_SHARED_LIBRARY)

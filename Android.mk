LOCAL_PATH:= $(call my-dir)

TINYPROXY_TOP := $(LOCAL_PATH)

include $(CLEAR-VARS)

include $(TINYPROXY_TOP)/src/Android.mk

PRODUCT_COPY_FILES += \
	$(TINYPROXY_TOP)/tinyproxy.conf:system/etc/tinyproxy.conf

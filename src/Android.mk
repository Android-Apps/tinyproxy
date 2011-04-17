LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE_TAGS:=eng debug

LOCAL_SRC_FILES:= \
	acl.c acl.h \
	anonymous.c anonymous.h \
	authors.c authors.h \
	buffer.c buffer.h \
	child.c child.h \
	common.h \
	conf.c conf.h \
	conns.c conns.h \
	daemon.c daemon.h \
	hashmap.c hashmap.h \
	heap.c heap.h \
	html-error.c html-error.h \
	http-message.c http-message.h \
	log.c log.h \
	network.c network.h \
	reqs.c reqs.h \
	sock.c sock.h \
	stats.c stats.h \
	text.c text.h \
	main.c main.h \
	utils.c utils.h \
	vector.c vector.h \
	upstream.c upstream.h \
	connect-ports.c connect-ports.h \
	filter.c filter.h \
	transparent-proxy.c transparent-proxy.h

#LOCAL_SHARED_LIBRARIES := librtmp

LOCAL_MODULE:= tinyproxy

LOCAL_C_INCLUDES := 			\
	$(TINYPROXY_TOP)			\
	$(LOCAL_PATH)

LOCAL_CFLAGS := \
	-DHAVE_CONFIG_H

include $(BUILD_EXECUTABLE)

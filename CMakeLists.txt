cmake_minimum_required(VERSION 3.28)

project(winRemoteControl LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

if(NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_PREFIX_PATH "D:/Qt/6.10.2/msvc2022_64" CACHE PATH "Qt 6 MSVC 2022 path")

find_package(Qt6 REQUIRED COMPONENTS Widgets Gui Network)

include(CTest)

set(WEBVIEW2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2")
set(FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg")
set(WEBRTC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webrtc/src")
set(WEBRTC_OUT "${WEBRTC_ROOT}/out/wrc_release_x64_msvcstl")

add_library(wrc_protocol STATIC
	src/core/protocol/inputmessage.cpp
	src/core/protocol/inputmessage.h
	src/core/protocol/sessionmessage.cpp
	src/core/protocol/sessionmessage.h
)

target_include_directories(wrc_protocol PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(wrc_protocol PUBLIC
	Qt6::Core
)

add_library(wrc_core STATIC
	src/core/session/sessionstatemachine.cpp
	src/core/session/sessionstatemachine.h
)

target_include_directories(wrc_core PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(wrc_core PUBLIC
	Qt6::Core
)

qt_add_executable(winRemoteControl WIN32
	src/app/main.cpp
	src/app/mainwindow.cpp
	src/app/mainwindow.h
	src/app/remotedesktopwindow.cpp
	src/app/remotedesktopwindow.h
	src/common/latencytracelogger.cpp
	src/common/latencytracelogger.h
	src/common/framewatermark.cpp
	src/common/framewatermark.h
	src/common/sessiontracelogger.cpp
	src/common/sessiontracelogger.h
	src/common/streamconfig.h
	src/codec/decodedvideoframe.h
	src/codec/h264decoder.cpp
	src/codec/h264decoder.h
	src/codec/h264encoder.cpp
	src/codec/h264encoder.h
	src/capture/captureframe.h
	src/capture/captureservice.cpp
	src/capture/captureservice.h
	src/capture/captureworker.cpp
	src/capture/captureworker.h
	src/capture/dxgidesktopduplicator.cpp
	src/capture/dxgidesktopduplicator.h
	src/input/inputinjector.cpp
	src/input/inputinjector.h
	src/render/videorenderwidget.cpp
	src/render/videorenderwidget.h
	src/session/sessionviewmodel.cpp
	src/session/sessionviewmodel.h
	src/transport/webrtc/webrtcpeer.cpp
	src/transport/webrtc/webrtcpeer.h
	src/transport/webrtc/webrtch264decoder.cpp
	src/transport/webrtc/webrtch264decoder.h
	src/transport/webrtc/webrtch264encoder.cpp
	src/transport/webrtc/webrtch264encoder.h
	src/transport/webrtc/webrtcnetworkstats.h
	src/transport/webrtc/webrtcsessionservice.cpp
	src/transport/webrtc/webrtcsessionservice.h
	src/transport/webrtc/webrtcsignaling.cpp
	src/transport/webrtc/webrtcsignaling.h
	src/transport/webrtc/webrtcvideoframe.h
	src/ui_bridge/webviewwidget.cpp
	src/ui_bridge/webviewwidget.h
)

target_compile_definitions(winRemoteControl PRIVATE
	UNICODE
	_UNICODE
	WIN32_LEAN_AND_MEAN
	NOMINMAX
	WEBRTC_WIN
	WEBRTC_USE_H264
	WEBRTC_ENABLE_PROTOBUF=0
)

target_include_directories(winRemoteControl PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}/src"
	"${WEBVIEW2_ROOT}/build/native/include"
	"${FFMPEG_ROOT}/include"
	"${WEBRTC_ROOT}"
	"${WEBRTC_ROOT}/third_party/abseil-cpp"
	"${WEBRTC_ROOT}/third_party/libyuv/include"
	"${WEBRTC_OUT}/gen"
)

target_link_directories(winRemoteControl PRIVATE
	"${FFMPEG_ROOT}/lib"
)

target_link_libraries(winRemoteControl PRIVATE
	wrc_core
	wrc_protocol
	Qt6::Widgets
	Qt6::Gui
	Qt6::Network
	d3d11
	dxgi
	dxguid
	d3dcompiler
	ole32
	shlwapi
	avcodec
	avutil
	swscale
	"${WEBRTC_OUT}/obj/webrtc.lib"
	ws2_32
	winmm
	secur32
	crypt32
	iphlpapi
	bcrypt
	dmoguids
	wmcodecdspuuid
	msdmo
	strmiids
	"${WEBVIEW2_ROOT}/build/native/x64/WebView2Loader.dll.lib"
)

if(BUILD_TESTING)
	add_executable(wrc_protocol_tests
		tests/unit/protocolcodectest.cpp
	)
	target_link_libraries(wrc_protocol_tests PRIVATE
		wrc_protocol
		Qt6::Core
	)
	add_test(NAME wrc_protocol_tests COMMAND wrc_protocol_tests)

	add_executable(wrc_session_state_tests
		tests/unit/sessionstatemachinetest.cpp
	)
	target_link_libraries(wrc_session_state_tests PRIVATE
		wrc_core
		Qt6::Core
	)
	add_test(NAME wrc_session_state_tests COMMAND wrc_session_state_tests)
endif()

set(WEBVIEW2_LOADER "${WEBVIEW2_ROOT}/build/native/x64/WebView2Loader.dll")
set(FRONTEND_DIST "${CMAKE_CURRENT_SOURCE_DIR}/frontend/dist")
set(FFMPEG_DLLS
	"${FFMPEG_ROOT}/bin/avcodec-62.dll"
	"${FFMPEG_ROOT}/bin/avutil-60.dll"
	"${FFMPEG_ROOT}/bin/swscale-9.dll"
	"${FFMPEG_ROOT}/bin/swresample-6.dll"
)

add_custom_command(TARGET winRemoteControl POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"${WEBVIEW2_LOADER}"
		"$<TARGET_FILE_DIR:winRemoteControl>/WebView2Loader.dll"
	COMMAND ${CMAKE_COMMAND} -E make_directory
		"$<TARGET_FILE_DIR:winRemoteControl>/frontend"
	COMMAND ${CMAKE_COMMAND} -E copy_directory
		"${FRONTEND_DIST}"
		"$<TARGET_FILE_DIR:winRemoteControl>/frontend"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		${FFMPEG_DLLS}
		"$<TARGET_FILE_DIR:winRemoteControl>"
	COMMAND "D:/Qt/6.10.2/msvc2022_64/bin/windeployqt.exe"
		--release
		--no-translations
		--no-compiler-runtime
		"$<TARGET_FILE:winRemoteControl>"
	VERBATIM
)

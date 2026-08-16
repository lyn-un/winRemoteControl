cmake_minimum_required(VERSION 3.28)

project(winRemoteControl LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

option(WRC_BUILD_APP "Build the Windows desktop application" ON)
option(WRC_BUILD_TESTS "Build automated tests" ON)
option(WRC_ENABLE_WEBRTC "Enable the WebRTC transport adapter" ON)
option(WRC_ENABLE_FFMPEG "Enable the FFmpeg codec adapter" ON)
option(WRC_ENABLE_WEBVIEW2 "Enable the WebView2 presentation adapter" ON)

if(NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(NOT CMAKE_PREFIX_PATH AND DEFINED ENV{QTDIR})
	list(PREPEND CMAKE_PREFIX_PATH "$ENV{QTDIR}")
endif()

find_package(Qt6 REQUIRED COMPONENTS Core)
if(WRC_BUILD_APP)
	if(NOT WIN32)
		message(FATAL_ERROR "WRC_BUILD_APP currently supports Windows only")
	endif()
	if(NOT WRC_ENABLE_WEBRTC OR NOT WRC_ENABLE_FFMPEG OR NOT WRC_ENABLE_WEBVIEW2)
		message(FATAL_ERROR
			"The desktop application currently requires WebRTC, FFmpeg and WebView2. "
			"Disable WRC_BUILD_APP for a dependency-light core build.")
	endif()
	find_package(Qt6 REQUIRED COMPONENTS Widgets Gui Network)
endif()

include(CTest)
set(BUILD_TESTING ${WRC_BUILD_TESTS} CACHE BOOL "Build tests" FORCE)

set(WEBVIEW2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2"
	CACHE PATH "WebView2 SDK root")
set(FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg"
	CACHE PATH "FFmpeg SDK root")
set(WEBRTC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webrtc/src"
	CACHE PATH "WebRTC source root")
set(WEBRTC_OUT "${WEBRTC_ROOT}/out/wrc_release_x64_msvcstl"
	CACHE PATH "WebRTC build output root")

add_library(wrc_foundation INTERFACE)

target_sources(wrc_foundation INTERFACE
	src/core/devices/recentdevice.h
	src/core/media/decodedvideoframe.h
	src/core/media/networkstats.h
	src/core/media/streamconfig.h
	src/core/media/videoframe.h
	src/core/settings/applicationsettings.h
)

target_include_directories(wrc_foundation INTERFACE
	"${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(wrc_foundation INTERFACE
	Qt6::Core
)

add_library(wrc_protocol STATIC
	src/core/protocol/accessmessage.cpp
	src/core/protocol/accessmessage.h
	src/core/protocol/clipboardmessage.cpp
	src/core/protocol/clipboardmessage.h
	src/core/protocol/landiscoverymessage.cpp
	src/core/protocol/landiscoverymessage.h
	src/core/protocol/protocolenvelope.cpp
	src/core/protocol/protocolenvelope.h
	src/core/protocol/protocolconstraints.h
	src/core/protocol/protocolrouter.cpp
	src/core/protocol/protocolrouter.h
	src/core/protocol/inputmessage.cpp
	src/core/protocol/inputmessage.h
	src/core/protocol/identitymessage.cpp
	src/core/protocol/identitymessage.h
	src/core/protocol/sessionmessage.cpp
	src/core/protocol/sessionmessage.h
	src/core/protocol/terminalmessage.cpp
	src/core/protocol/terminalmessage.h
	src/core/protocol/terminaldataframe.cpp
	src/core/protocol/terminaldataframe.h
	src/core/protocol/webrtcsignalingmessage.cpp
	src/core/protocol/webrtcsignalingmessage.h
)

target_include_directories(wrc_protocol PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(wrc_protocol PUBLIC
	wrc_foundation
	Qt6::Core
)

add_library(wrc_core STATIC
	src/core/security/devicecertificate.cpp
	src/core/security/devicecertificate.h
	src/core/security/deviceidentity.cpp
	src/core/security/deviceidentity.h
	src/core/security/deviceidentityprovider.h
	src/core/security/permissionscope.cpp
	src/core/security/permissionscope.h
	src/core/security/securitycanonicalwriter.cpp
	src/core/security/securitycanonicalwriter.h
	src/core/security/trusteddevice.h
	src/core/security/trusteddevicestore.h
	src/core/session/capabilitynegotiator.cpp
	src/core/session/capabilitynegotiator.h
	src/core/clipboard/clipboardadapter.h
	src/core/devices/recentdevicestore.h
	src/core/discovery/devicediscoverycontroller.h
	src/core/discovery/discovereddevice.h
	src/core/discovery/landiscoverytransport.h
	src/core/input/inputinjectorinterface.h
	src/core/media/captureframe.h
	src/core/media/captureframesink.h
	src/core/media/capturesource.h
	src/core/media/capturecontroller.h
	src/core/session/deviceinfoprovider.h
	src/core/session/sessionerror.cpp
	src/core/session/sessionerror.h
	src/core/session/sessionstatemachine.cpp
	src/core/session/sessionstatemachine.h
	src/core/settings/applicationsettings.cpp
	src/core/settings/applicationsettingsstore.h
	src/core/transport/remotepeertransport.h
	src/core/transport/signalingtransport.h
	src/core/transport/tlspeeridentity.cpp
	src/core/transport/tlspeeridentity.h
	src/core/transport/networkstatstracker.cpp
	src/core/transport/networkstatstracker.h
	src/core/transport/outboundmessagequeue.cpp
	src/core/transport/outboundmessagequeue.h
	src/core/transport/peerinitializationresult.cpp
	src/core/transport/peerinitializationresult.h
	src/core/terminal/terminalhost.h
	src/core/terminal/terminalfrontend.h
	src/core/terminal/terminalrelayframecodec.cpp
	src/core/terminal/terminalrelayframecodec.h
	src/core/terminal/terminalrelayprotocol.h
	src/core/terminal/terminalstate.h
	src/core/terminal/terminalstatemachine.cpp
	src/core/terminal/terminalstatemachine.h
	src/core/terminal/terminalwriteall.cpp
	src/core/terminal/terminalwriteall.h
)

target_include_directories(wrc_core PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(wrc_core PUBLIC
	wrc_foundation
	wrc_protocol
	Qt6::Core
)

if(WIN32)
add_library(wrc_windows_security_adapter STATIC
	src/adapters/windows/security/atomicjsonfile.cpp
	src/adapters/windows/security/atomicjsonfile.h
	src/adapters/windows/security/signedjsontrusteddevicestore.cpp
	src/adapters/windows/security/signedjsontrusteddevicestore.h
	src/adapters/windows/security/windowsdeviceidentityprovider.cpp
	src/adapters/windows/security/windowsdeviceidentityprovider.h
)

target_compile_definitions(wrc_windows_security_adapter PRIVATE
	UNICODE
	_UNICODE
	WIN32_LEAN_AND_MEAN
	NOMINMAX
)

target_include_directories(wrc_windows_security_adapter PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(wrc_windows_security_adapter PUBLIC
	wrc_core
	Qt6::Core
	bcrypt
	ncrypt
	crypt32
)
endif()

add_library(wrc_diagnostics STATIC
	src/common/latencytracelogger.cpp
	src/common/latencytracelogger.h
	src/common/sessiontracelogger.cpp
	src/common/sessiontracelogger.h
	src/common/traceoptions.cpp
	src/common/traceoptions.h
)

target_include_directories(wrc_diagnostics PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(wrc_diagnostics PUBLIC
	Qt6::Core
)

add_library(wrc_session_contract STATIC
	src/session/sessioncontroller.h
)

target_include_directories(wrc_session_contract PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(wrc_session_contract PUBLIC
	wrc_core
	wrc_protocol
	Qt6::Core
)

if(WRC_BUILD_APP)
add_library(wrc_windows_adapters STATIC
	src/adapters/clipboard/clipboardchangefilter.cpp
	src/adapters/clipboard/clipboardchangefilter.h
	src/adapters/clipboard/qtclipboardadapter.cpp
	src/adapters/clipboard/qtclipboardadapter.h
	src/adapters/windows/device/windowsdeviceinfoprovider.cpp
	src/adapters/windows/device/windowsdeviceinfoprovider.h
	src/adapters/windows/input/windowsinputinjector.cpp
	src/adapters/windows/input/windowsinputinjector.h
)

target_compile_definitions(wrc_windows_adapters PRIVATE
	UNICODE
	_UNICODE
	WIN32_LEAN_AND_MEAN
	NOMINMAX
)

target_include_directories(wrc_windows_adapters PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(wrc_windows_adapters PUBLIC
	wrc_core
	wrc_diagnostics
	Qt6::Core
	Qt6::Gui
	user32
)
add_library(wrc_session_application STATIC
	src/input/inputinjector.cpp
	src/input/inputinjector.h
	src/session/authenticatedsignalingflow.cpp
	src/session/authenticatedsignalingflow.h
	src/session/deviceauthenticationflow.cpp
	src/session/deviceauthenticationflow.h
	src/session/accessapprovalcontroller.cpp
	src/session/accessapprovalcontroller.h
	src/session/accesssessionflow.cpp
	src/session/accesssessionflow.h
	src/session/capabilitysessionflow.cpp
	src/session/capabilitysessionflow.h
	src/session/mediasessioncontroller.cpp
	src/session/mediasessioncontroller.h
	src/session/peerlifecyclecontroller.cpp
	src/session/peerlifecyclecontroller.h
	src/session/sessioncoordinator.cpp
	src/session/sessioncoordinator.h
	src/session/sessioncommanddispatcher.cpp
	src/session/sessioncommanddispatcher.h
	src/session/recoverycontroller.cpp
	src/session/recoverycontroller.h
	src/session/sessionerrorpresenter.cpp
	src/session/sessionerrorpresenter.h
	src/session/shutdowncoordinator.cpp
	src/session/shutdowncoordinator.h
)

target_include_directories(wrc_session_application PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(wrc_session_application PUBLIC
	wrc_core
	wrc_protocol
	wrc_session_contract
	wrc_diagnostics
	Qt6::Core
)

add_library(wrc_signaling_adapter STATIC
	src/adapters/signaling/schanneltlsengine.cpp
	src/adapters/signaling/schanneltlsengine.h
	src/adapters/signaling/tcpsignalingtransport.cpp
	src/adapters/signaling/tcpsignalingtransport.h
)

add_library(wrc_discovery_application STATIC
	src/discovery/landiscoveryservice.cpp
	src/discovery/landiscoveryservice.h
)

target_include_directories(wrc_discovery_application PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(wrc_discovery_application PUBLIC
	wrc_core
	wrc_protocol
	wrc_diagnostics
	Qt6::Core
)

add_library(wrc_recent_device_application STATIC
	src/devices/recentdeviceservice.cpp
	src/devices/recentdeviceservice.h
)

add_library(wrc_settings_application STATIC
	src/settings/applicationsettingsservice.cpp
	src/settings/applicationsettingsservice.h
)

add_library(wrc_clipboard_application STATIC
	src/clipboard/clipboardsyncservice.cpp
	src/clipboard/clipboardsyncservice.h
)

endif() # WRC_BUILD_APP

add_library(wrc_terminal_application STATIC
	src/terminal/terminalcommanddispatcher.cpp
	src/terminal/terminalcommanddispatcher.h
	src/terminal/terminalsessionservice.cpp
	src/terminal/terminalsessionservice.h
)

target_include_directories(wrc_terminal_application PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(wrc_terminal_application PUBLIC
	wrc_core
	wrc_session_contract
	wrc_diagnostics
	Qt6::Core
)

add_library(wrc_windows_terminal_adapter STATIC
	src/adapters/windows/terminal/powershelllaunchpolicy.cpp
	src/adapters/windows/terminal/powershelllaunchpolicy.h
	src/adapters/windows/terminal/windowspseudoconsole.cpp
	src/adapters/windows/terminal/windowspseudoconsole.h
)

target_compile_definitions(wrc_windows_terminal_adapter PRIVATE
	UNICODE
	_UNICODE
	WIN32_LEAN_AND_MEAN
	NOMINMAX
)

target_include_directories(wrc_windows_terminal_adapter PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(wrc_windows_terminal_adapter PUBLIC
	wrc_core
	wrc_diagnostics
	Qt6::Core
)

if(WRC_BUILD_APP)
add_library(wrc_windows_terminal_frontend_adapter STATIC
	src/adapters/windows/terminal/windowsterminalfrontend.cpp
	src/adapters/windows/terminal/windowsterminalfrontend.h
)

target_compile_definitions(wrc_windows_terminal_frontend_adapter PRIVATE
	UNICODE
	_UNICODE
	WIN32_LEAN_AND_MEAN
	NOMINMAX
)

target_include_directories(wrc_windows_terminal_frontend_adapter PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(wrc_windows_terminal_frontend_adapter PUBLIC
	wrc_core
	wrc_diagnostics
	Qt6::Core
	Qt6::Network
)

add_executable(wrcTerminalRelay
	src/terminal_relay/main.cpp
)

set_target_properties(wrcTerminalRelay PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/build/Release"
)

target_compile_definitions(wrcTerminalRelay PRIVATE
	UNICODE
	_UNICODE
	WIN32_LEAN_AND_MEAN
	NOMINMAX
)

target_include_directories(wrcTerminalRelay PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_include_directories(wrc_clipboard_application PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(wrc_clipboard_application PUBLIC
	wrc_core
	wrc_session_application
	wrc_diagnostics
	Qt6::Core
)

target_include_directories(wrc_settings_application PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(wrc_settings_application PUBLIC
	wrc_core
	wrc_diagnostics
	Qt6::Core
)

target_include_directories(wrc_recent_device_application PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(wrc_recent_device_application PUBLIC
	wrc_core
	wrc_diagnostics
	Qt6::Core
)

add_library(wrc_settings_adapter STATIC
	src/adapters/settings/qsettingsapplicationstore.cpp
	src/adapters/settings/qsettingsapplicationstore.h
	src/adapters/settings/qsettingsrecentdevicestore.cpp
	src/adapters/settings/qsettingsrecentdevicestore.h
)

target_include_directories(wrc_settings_adapter PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(wrc_settings_adapter PUBLIC
	wrc_core
	Qt6::Core
)

add_library(wrc_udp_discovery_adapter STATIC
	src/adapters/discovery/udplandiscoverytransport.cpp
	src/adapters/discovery/udplandiscoverytransport.h
)

target_include_directories(wrc_udp_discovery_adapter PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(wrc_udp_discovery_adapter PUBLIC
	wrc_core
	Qt6::Core
	Qt6::Network
	ws2_32
)

target_include_directories(wrc_signaling_adapter PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_definitions(wrc_signaling_adapter PRIVATE SECURITY_WIN32)

target_link_libraries(wrc_signaling_adapter PUBLIC
	wrc_core
	wrc_diagnostics
	Qt6::Core
	Qt6::Network
	secur32
	crypt32
)

add_library(wrc_ffmpeg_adapter STATIC
	src/codec/h264decoder.cpp
	src/codec/h264decoder.h
	src/codec/h264encoder.cpp
	src/codec/h264encoder.h
)

target_include_directories(wrc_ffmpeg_adapter PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_include_directories(wrc_ffmpeg_adapter PRIVATE
	"${FFMPEG_ROOT}/include"
	"${WEBRTC_ROOT}"
	"${WEBRTC_ROOT}/third_party/libyuv/include"
)

target_link_directories(wrc_ffmpeg_adapter PUBLIC
	"${FFMPEG_ROOT}/lib"
)

target_link_libraries(wrc_ffmpeg_adapter PUBLIC
	wrc_foundation
	Qt6::Core
	avcodec
	avutil
	swscale
	"${WEBRTC_OUT}/obj/webrtc.lib"
)

add_library(wrc_webrtc_adapter STATIC
	src/transport/webrtc/webrtccallbackgate.cpp
	src/transport/webrtc/webrtccallbackgate.h
	src/transport/webrtc/webrtcpeer.cpp
	src/transport/webrtc/webrtcpeer.h
	src/transport/webrtc/webrtcdatachannel.cpp
	src/transport/webrtc/webrtcdatachannel.h
	src/transport/webrtc/webrtclatencyprobe.cpp
	src/transport/webrtc/webrtclatencyprobe.h
	src/transport/webrtc/webrtcremoteframeprocessor.cpp
	src/transport/webrtc/webrtcremoteframeprocessor.h
	src/transport/webrtc/webrtch264decoder.cpp
	src/transport/webrtc/webrtch264decoder.h
	src/transport/webrtc/webrtch264encoder.cpp
	src/transport/webrtc/webrtch264encoder.h
)

target_compile_definitions(wrc_webrtc_adapter PUBLIC
	WEBRTC_WIN
	WEBRTC_USE_H264
	WEBRTC_ENABLE_PROTOBUF=0
	UNICODE
	_UNICODE
	WIN32_LEAN_AND_MEAN
	NOMINMAX
)

target_include_directories(wrc_webrtc_adapter PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/src"
	"${FFMPEG_ROOT}/include"
	"${WEBRTC_ROOT}"
	"${WEBRTC_ROOT}/third_party/abseil-cpp"
	"${WEBRTC_ROOT}/third_party/libyuv/include"
	"${WEBRTC_OUT}/gen"
)

target_link_libraries(wrc_webrtc_adapter PUBLIC
	wrc_core
	wrc_protocol
	wrc_ffmpeg_adapter
	wrc_diagnostics
	Qt6::Core
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
)

add_library(wrc_windows_capture_adapter STATIC
	src/capture/dxgidesktopduplicator.cpp
	src/capture/dxgidesktopduplicator.h
)

target_compile_definitions(wrc_windows_capture_adapter PRIVATE
	UNICODE
	_UNICODE
	WIN32_LEAN_AND_MEAN
	NOMINMAX
)

target_include_directories(wrc_windows_capture_adapter PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(wrc_windows_capture_adapter PUBLIC
	wrc_core
	Qt6::Core
	Qt6::Gui
	d3d11
	dxgi
	dxguid
)

add_library(wrc_capture_application STATIC
	src/common/framewatermark.cpp
	src/common/framewatermark.h
	src/capture/captureframesink.cpp
	src/capture/captureframesink.h
	src/capture/captureservice.cpp
	src/capture/captureservice.h
	src/capture/captureworker.cpp
	src/capture/captureworker.h
)

target_compile_definitions(wrc_capture_application PRIVATE
	UNICODE
	_UNICODE
	WIN32_LEAN_AND_MEAN
	NOMINMAX
)

target_include_directories(wrc_capture_application PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_include_directories(wrc_capture_application PRIVATE
	"${FFMPEG_ROOT}/include"
)

target_link_libraries(wrc_capture_application PUBLIC
	wrc_core
	wrc_ffmpeg_adapter
	wrc_windows_capture_adapter
	wrc_diagnostics
	Qt6::Core
	Qt6::Gui
	d3d11
	dxgi
	dxguid
)

add_library(wrc_qt_presentation STATIC
	src/app/remotedesktopwindow.cpp
	src/app/remotedesktopwindow.h
	src/render/videorenderwidget.cpp
	src/render/videorenderwidget.h
	src/session/inputfeedbacktracker.cpp
	src/session/inputfeedbacktracker.h
	src/session/sessionviewmodel.cpp
	src/session/sessionviewmodel.h
	src/ui_bridge/webviewwidget.cpp
	src/ui_bridge/webviewwidget.h
	src/ui_bridge/devicediscoveryviewmodel.cpp
	src/ui_bridge/devicediscoveryviewmodel.h
)

target_compile_definitions(wrc_qt_presentation PRIVATE
	UNICODE
	_UNICODE
	WIN32_LEAN_AND_MEAN
	NOMINMAX
)

target_include_directories(wrc_qt_presentation PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/src"
	"${WEBVIEW2_ROOT}/build/native/include"
)

target_link_libraries(wrc_qt_presentation PUBLIC
	wrc_core
	wrc_session_application
	wrc_clipboard_application
	wrc_terminal_application
	wrc_diagnostics
	Qt6::Widgets
	Qt6::Gui
	d3d11
	dxgi
	dxguid
	d3dcompiler
	ole32
	shlwapi
	"${WEBVIEW2_ROOT}/build/native/x64/WebView2Loader.dll.lib"
)

set(WRC_APP_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/assets/winremotecontrol.ico")
configure_file(
	"${CMAKE_CURRENT_SOURCE_DIR}/src/app/winremotecontrol.rc.in"
	"${CMAKE_CURRENT_BINARY_DIR}/generated/winremotecontrol.rc"
	@ONLY
)

qt_add_executable(winRemoteControl WIN32
	src/app/main.cpp
	src/app/composition/applicationcomposition.cpp
	src/app/composition/applicationcomposition.h
	src/app/mainwindow.cpp
	src/app/mainwindow.h
	"${CMAKE_CURRENT_BINARY_DIR}/generated/winremotecontrol.rc"
)

qt_add_resources(winRemoteControl "branding"
	PREFIX "/branding"
	BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets"
	FILES
		assets/app-icon.png
)

add_dependencies(winRemoteControl wrcTerminalRelay)

set_target_properties(winRemoteControl PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/build/Release"
)

target_compile_definitions(winRemoteControl PRIVATE
	UNICODE
	_UNICODE
	WIN32_LEAN_AND_MEAN
	NOMINMAX
)

target_include_directories(winRemoteControl PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(winRemoteControl PRIVATE
	wrc_core
	wrc_protocol
	wrc_diagnostics
	wrc_session_application
	wrc_signaling_adapter
	wrc_discovery_application
	wrc_recent_device_application
	wrc_settings_application
	wrc_settings_adapter
	wrc_udp_discovery_adapter
	wrc_webrtc_adapter
	wrc_windows_adapters
	wrc_windows_security_adapter
	wrc_windows_terminal_adapter
	wrc_windows_terminal_frontend_adapter
	wrc_capture_application
	wrc_windows_capture_adapter
	wrc_qt_presentation
	Qt6::Widgets
	Qt6::Gui
	Qt6::Network
	dwmapi
	ole32
)

endif() # WRC_BUILD_APP

if(WRC_BUILD_TESTS)
	if(WIN32)
	add_executable(wrc_device_identity_tests
		tests/unit/deviceidentitytest.cpp
	)
	target_link_libraries(wrc_device_identity_tests PRIVATE
		wrc_windows_security_adapter
		Qt6::Core
	)
	add_test(NAME wrc_device_identity_tests COMMAND wrc_device_identity_tests)
	set_tests_properties(wrc_device_identity_tests PROPERTIES
		ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
	)
	endif()

	add_executable(wrc_protocol_tests
		tests/unit/protocolcodectest.cpp
	)
	target_link_libraries(wrc_protocol_tests PRIVATE
		wrc_protocol
		Qt6::Core
	)
	add_test(NAME wrc_protocol_tests COMMAND wrc_protocol_tests)

	add_executable(wrc_protocol_robustness_tests
		tests/unit/protocolrobustnesstest.cpp
	)
	target_link_libraries(wrc_protocol_robustness_tests PRIVATE
		wrc_protocol
		Qt6::Core
	)
	add_test(NAME wrc_protocol_robustness_tests COMMAND wrc_protocol_robustness_tests)

	add_executable(wrc_session_state_tests
		tests/unit/sessionstatemachinetest.cpp
	)
	target_link_libraries(wrc_session_state_tests PRIVATE
		wrc_core
		Qt6::Core
	)
	add_test(NAME wrc_session_state_tests COMMAND wrc_session_state_tests)

	add_executable(wrc_network_stats_tests
		tests/unit/networkstatstrackertest.cpp
	)
	target_link_libraries(wrc_network_stats_tests PRIVATE
		wrc_core
		Qt6::Core
	)
	add_test(NAME wrc_network_stats_tests COMMAND wrc_network_stats_tests)

	add_executable(wrc_terminal_write_all_tests
		tests/unit/terminalwritealltest.cpp
	)
	target_link_libraries(wrc_terminal_write_all_tests PRIVATE
		wrc_core
		Qt6::Core
	)
	add_test(NAME wrc_terminal_write_all_tests COMMAND wrc_terminal_write_all_tests)

	add_executable(wrc_terminal_state_machine_tests
		tests/unit/terminalstatemachinetest.cpp
	)
	target_link_libraries(wrc_terminal_state_machine_tests PRIVATE
		wrc_core
		Qt6::Core
	)
	add_test(NAME wrc_terminal_state_machine_tests COMMAND wrc_terminal_state_machine_tests)

	add_executable(wrc_p1_core_tests
		tests/unit/p1coretest.cpp
	)
	target_link_libraries(wrc_p1_core_tests PRIVATE
		wrc_core
		wrc_protocol
		Qt6::Core
	)
	add_test(NAME wrc_p1_core_tests COMMAND wrc_p1_core_tests)

	add_executable(wrc_webrtc_callback_gate_tests
		tests/unit/webrtccallbackgatetest.cpp
		src/transport/webrtc/webrtccallbackgate.cpp
	)
	target_include_directories(wrc_webrtc_callback_gate_tests PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/src"
	)
	target_link_libraries(wrc_webrtc_callback_gate_tests PRIVATE
		Qt6::Core
	)
	add_test(NAME wrc_webrtc_callback_gate_tests
		COMMAND wrc_webrtc_callback_gate_tests)
	set_tests_properties(
		wrc_protocol_tests
		wrc_protocol_robustness_tests
		wrc_session_state_tests
		wrc_network_stats_tests
		wrc_terminal_write_all_tests
		wrc_terminal_state_machine_tests
		wrc_p1_core_tests
		wrc_webrtc_callback_gate_tests
		PROPERTIES ENVIRONMENT_MODIFICATION
			"Path=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
	)

	if(WRC_BUILD_APP)
	add_executable(wrc_input_injector_tests
		tests/unit/inputinjectortest.cpp
	)
	target_link_libraries(wrc_input_injector_tests PRIVATE
		wrc_session_application
		Qt6::Core
	)
	add_test(NAME wrc_input_injector_tests COMMAND wrc_input_injector_tests)

	add_executable(wrc_webrtc_peer_initialization_tests
		tests/unit/webrtcpeerinitializationtest.cpp
		src/common/framewatermark.cpp
	)
	target_link_libraries(wrc_webrtc_peer_initialization_tests PRIVATE
		wrc_webrtc_adapter
		Qt6::Core
	)
	add_test(NAME wrc_webrtc_peer_initialization_tests
		COMMAND wrc_webrtc_peer_initialization_tests)

	add_executable(wrc_capture_pipeline_tests
		tests/unit/capturepipelinetest.cpp
	)
	target_link_libraries(wrc_capture_pipeline_tests PRIVATE
		wrc_capture_application
		Qt6::Core
	)
	add_test(NAME wrc_capture_pipeline_tests COMMAND wrc_capture_pipeline_tests)

	add_executable(wrc_video_pipeline_benchmark
		tests/performance/videopipelinebenchmark.cpp
	)
	target_include_directories(wrc_video_pipeline_benchmark PRIVATE
		"${FFMPEG_ROOT}/include"
	)
	target_link_libraries(wrc_video_pipeline_benchmark PRIVATE
		wrc_capture_application
		wrc_ffmpeg_adapter
		Qt6::Core
		Psapi
	)
	add_custom_command(TARGET wrc_video_pipeline_benchmark POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"${FFMPEG_ROOT}/bin/avcodec-62.dll"
			"${FFMPEG_ROOT}/bin/avutil-60.dll"
			"${FFMPEG_ROOT}/bin/swscale-9.dll"
			"${FFMPEG_ROOT}/bin/swresample-6.dll"
			"$<TARGET_FILE_DIR:wrc_video_pipeline_benchmark>"
		COMMAND "$<TARGET_FILE:Qt6::windeployqt>"
			--release
			--no-translations
			--no-compiler-runtime
			"$<TARGET_FILE:wrc_video_pipeline_benchmark>"
		VERBATIM
	)

	add_executable(wrc_session_service_tests
		tests/unit/sessionservicetest.cpp
	)
	target_link_libraries(wrc_session_service_tests PRIVATE
		wrc_session_application
		wrc_signaling_adapter
		Qt6::Core
		Qt6::Network
	)
	add_test(NAME wrc_session_service_tests COMMAND wrc_session_service_tests)

	add_executable(wrc_peer_lifecycle_controller_tests
		tests/unit/peerlifecyclecontrollertest.cpp
	)
	target_link_libraries(wrc_peer_lifecycle_controller_tests PRIVATE
		wrc_session_application
		Qt6::Core
	)
	add_test(NAME wrc_peer_lifecycle_controller_tests
		COMMAND wrc_peer_lifecycle_controller_tests)

	add_executable(wrc_session_flow_controller_tests
		tests/unit/sessionflowcontrollertest.cpp
	)
	target_link_libraries(wrc_session_flow_controller_tests PRIVATE
		wrc_session_application
		Qt6::Core
	)
	add_test(NAME wrc_session_flow_controller_tests
		COMMAND wrc_session_flow_controller_tests)
	set_tests_properties(wrc_session_service_tests PROPERTIES
		ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Network>"
	)

	add_executable(wrc_access_approval_tests
		tests/unit/accessapprovalcontrollertest.cpp
	)
	target_link_libraries(wrc_access_approval_tests PRIVATE
		wrc_session_application
		Qt6::Core
	)
	add_test(NAME wrc_access_approval_tests COMMAND wrc_access_approval_tests)

	add_executable(wrc_device_authentication_tests
		tests/unit/deviceauthenticationtest.cpp
	)
	target_link_libraries(wrc_device_authentication_tests PRIVATE
		wrc_session_application
		Qt6::Core
	)
	add_test(NAME wrc_device_authentication_tests
		COMMAND wrc_device_authentication_tests)

	add_executable(wrc_authenticated_signaling_tests
		tests/unit/authenticatedsignalingtest.cpp
	)
	target_link_libraries(wrc_authenticated_signaling_tests PRIVATE
		wrc_session_application
		Qt6::Core
	)
	add_test(NAME wrc_authenticated_signaling_tests
		COMMAND wrc_authenticated_signaling_tests)

	add_executable(wrc_session_command_dispatcher_tests
		tests/unit/sessioncommanddispatchertest.cpp
	)
	target_link_libraries(wrc_session_command_dispatcher_tests PRIVATE
		wrc_session_application
		Qt6::Core
	)
	add_test(NAME wrc_session_command_dispatcher_tests
		COMMAND wrc_session_command_dispatcher_tests)

	add_executable(wrc_shutdown_coordinator_tests
		tests/unit/shutdowncoordinatortest.cpp
	)
	target_link_libraries(wrc_shutdown_coordinator_tests PRIVATE
		wrc_session_application
		Qt6::Core
	)
	add_test(NAME wrc_shutdown_coordinator_tests
		COMMAND wrc_shutdown_coordinator_tests)

	add_executable(wrc_recovery_controller_tests
		tests/unit/recoverycontrollertest.cpp
	)
	target_link_libraries(wrc_recovery_controller_tests PRIVATE
		wrc_session_application
		Qt6::Core
	)
	add_test(NAME wrc_recovery_controller_tests
		COMMAND wrc_recovery_controller_tests)

	add_executable(wrc_lan_discovery_tests
		tests/unit/landiscoveryservicetest.cpp
	)
	target_link_libraries(wrc_lan_discovery_tests PRIVATE
		wrc_discovery_application
		wrc_udp_discovery_adapter
		Qt6::Core
		Qt6::Network
	)
	add_test(NAME wrc_lan_discovery_tests COMMAND wrc_lan_discovery_tests)
	set_tests_properties(wrc_lan_discovery_tests PROPERTIES
		ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Network>"
	)

	add_executable(wrc_recent_device_tests
		tests/unit/recentdeviceservicetest.cpp
	)
	target_link_libraries(wrc_recent_device_tests PRIVATE
		wrc_recent_device_application
		wrc_settings_adapter
		Qt6::Core
	)
	add_test(NAME wrc_recent_device_tests COMMAND wrc_recent_device_tests)

	add_executable(wrc_application_settings_tests
		tests/unit/applicationsettingstest.cpp
	)
	target_link_libraries(wrc_application_settings_tests PRIVATE
		wrc_settings_application
		wrc_settings_adapter
		Qt6::Core
	)
	add_test(NAME wrc_application_settings_tests COMMAND wrc_application_settings_tests)

	add_executable(wrc_clipboard_sync_tests
		tests/unit/clipboardsyncservicetest.cpp
	)
	target_link_libraries(wrc_clipboard_sync_tests PRIVATE
		wrc_clipboard_application
		Qt6::Core
	)
	add_test(NAME wrc_clipboard_sync_tests COMMAND wrc_clipboard_sync_tests)

	endif() # WRC_BUILD_APP

	add_executable(wrc_terminal_session_tests
		tests/unit/terminalsessionservicetest.cpp
	)
	target_link_libraries(wrc_terminal_session_tests PRIVATE
		wrc_terminal_application
		Qt6::Core
	)
	add_test(NAME wrc_terminal_session_tests COMMAND wrc_terminal_session_tests)

	add_executable(wrc_terminal_command_dispatcher_tests
		tests/unit/terminalcommanddispatchertest.cpp
	)
	target_link_libraries(wrc_terminal_command_dispatcher_tests PRIVATE
		wrc_terminal_application
		Qt6::Core
	)
	add_test(NAME wrc_terminal_command_dispatcher_tests
		COMMAND wrc_terminal_command_dispatcher_tests)

	add_executable(wrc_terminal_relay_frame_codec_tests
		tests/unit/terminalrelayframecodectest.cpp
	)
	target_link_libraries(wrc_terminal_relay_frame_codec_tests PRIVATE
		wrc_core
		Qt6::Core
	)
	add_test(NAME wrc_terminal_relay_frame_codec_tests
		COMMAND wrc_terminal_relay_frame_codec_tests)

	add_executable(wrc_windows_pseudoconsole_tests
		tests/unit/windowspseudoconsoletest.cpp
	)
	target_link_libraries(wrc_windows_pseudoconsole_tests PRIVATE
		wrc_windows_terminal_adapter
		Qt6::Core
	)
	add_test(NAME wrc_windows_pseudoconsole_tests
		COMMAND wrc_windows_pseudoconsole_tests)

	add_executable(wrc_terminal_shell_policy_tests
		tests/unit/powershelllaunchpolicytest.cpp
	)
	target_link_libraries(wrc_terminal_shell_policy_tests PRIVATE
		wrc_windows_terminal_adapter
		Qt6::Core
	)
	add_test(NAME wrc_terminal_shell_policy_tests
		COMMAND wrc_terminal_shell_policy_tests)

	# Keep the dependency-light terminal preset runnable without relying on the
	# caller to export QTDIR. GitHub Actions does export it, but local preset
	# runs should resolve the same Qt runtime from the imported target.
	set_tests_properties(
		wrc_terminal_write_all_tests
		wrc_terminal_state_machine_tests
		wrc_terminal_session_tests
		wrc_terminal_command_dispatcher_tests
		wrc_terminal_relay_frame_codec_tests
		wrc_windows_pseudoconsole_tests
		wrc_terminal_shell_policy_tests
		PROPERTIES
		ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
	)

	if(WRC_BUILD_APP)
	add_executable(wrc_clipboard_change_filter_tests
		tests/unit/clipboardchangefiltertest.cpp
	)
	target_link_libraries(wrc_clipboard_change_filter_tests PRIVATE
		wrc_windows_adapters
		Qt6::Core
	)
	add_test(NAME wrc_clipboard_change_filter_tests COMMAND wrc_clipboard_change_filter_tests)

	add_executable(wrc_trace_options_tests
		tests/unit/traceoptionstest.cpp
	)
	target_link_libraries(wrc_trace_options_tests PRIVATE
		wrc_diagnostics
		Qt6::Core
	)
	add_test(NAME wrc_trace_options_tests COMMAND wrc_trace_options_tests)

	add_executable(wrc_frame_watermark_tests
		tests/unit/framewatermarktest.cpp
		src/common/framewatermark.cpp
	)
	target_include_directories(wrc_frame_watermark_tests PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/src"
	)
	target_link_libraries(wrc_frame_watermark_tests PRIVATE
		Qt6::Core
	)
	add_test(NAME wrc_frame_watermark_tests COMMAND wrc_frame_watermark_tests)

	endif() # WRC_BUILD_APP

	# Keep file-writing tests deterministic and inside the build tree. This also
	# avoids depending on the user profile's temporary-directory permissions.
	set(WRC_TEST_TEMP_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-temp")
	file(MAKE_DIRECTORY "${WRC_TEST_TEMP_DIRECTORY}")
	get_property(WRC_REGISTERED_TESTS DIRECTORY PROPERTY TESTS)
	set_tests_properties(${WRC_REGISTERED_TESTS} PROPERTIES
		ENVIRONMENT "TEMP=${WRC_TEST_TEMP_DIRECTORY};TMP=${WRC_TEST_TEMP_DIRECTORY}"
	)
endif()

if(WRC_BUILD_APP)
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
	COMMAND "$<TARGET_FILE:Qt6::windeployqt>"
		--release
		--no-translations
		--no-compiler-runtime
		"$<TARGET_FILE:winRemoteControl>"
	VERBATIM
)
endif() # WRC_BUILD_APP

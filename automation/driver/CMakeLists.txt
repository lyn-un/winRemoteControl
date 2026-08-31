add_library(wrc_driver_base STATIC
	base/commandcontext.cpp
	base/commandcontext.h
	base/httpserver.cpp
	base/httpserver.h
	base/idempotencystore.cpp
	base/idempotencystore.h
	base/requestparser.cpp
	base/requestparser.h
	base/requestrouter.cpp
	base/requestrouter.h
	base/session.cpp
	base/session.h
	base/sessionmanager.cpp
	base/sessionmanager.h
	base/status.cpp
	base/status.h
)

target_include_directories(wrc_driver_base PUBLIC
	"${PROJECT_SOURCE_DIR}"
)

target_link_libraries(wrc_driver_base PUBLIC
	Qt6::Core
	Qt6::Network
)

if(WRC_BUILD_AUTOMATION_DRIVER)
	add_library(wrc_driver_app STATIC
		app/sessioncommands.cpp
		app/sessioncommands.h
		app/statecommands.cpp
		app/statecommands.h
		app/triggercommand.cpp
		app/triggercommand.h
		app/wrcroutes.cpp
		app/wrcroutes.h
		app/wrcdrivermodule.cpp
		app/wrcdrivermodule.h
	)

	target_include_directories(wrc_driver_app PUBLIC
		"${PROJECT_SOURCE_DIR}"
		"${PROJECT_SOURCE_DIR}/src"
	)

	target_link_libraries(wrc_driver_app PUBLIC
		wrc_driver_base
		Qt6::Core
		Qt6::Network
	)

	add_library(wrcdriver SHARED
		app/wrcdriverexports.cpp
	)

	target_include_directories(wrcdriver PRIVATE
		"${PROJECT_SOURCE_DIR}"
		"${PROJECT_SOURCE_DIR}/src"
	)

	target_link_libraries(wrcdriver PRIVATE
		wrc_driver_app
		Qt6::Core
		Qt6::Network
	)

	set_target_properties(wrcdriver PROPERTIES
		OUTPUT_NAME "wrcdriver"
		RUNTIME_OUTPUT_DIRECTORY_RELEASE
			"${WRC_RUNTIME_OUTPUT_DIRECTORY}/automation"
	)
endif()

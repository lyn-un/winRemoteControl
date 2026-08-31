if(NOT DEFINED WRC_RUNTIME_DIRECTORY OR WRC_RUNTIME_DIRECTORY STREQUAL "")
	message(FATAL_ERROR "WRC_RUNTIME_DIRECTORY is required")
endif()

if(NOT EXISTS "${WRC_RUNTIME_DIRECTORY}/winRemoteControl.exe")
	message(FATAL_ERROR "Production runtime does not contain winRemoteControl.exe")
endif()

file(GLOB_RECURSE WRC_RUNTIME_FILES
	LIST_DIRECTORIES false
	RELATIVE "${WRC_RUNTIME_DIRECTORY}"
	"${WRC_RUNTIME_DIRECTORY}/*")

foreach(WRC_RUNTIME_FILE IN LISTS WRC_RUNTIME_FILES)
	string(REPLACE "\\" "/" WRC_NORMALIZED_FILE "${WRC_RUNTIME_FILE}")
	string(TOLOWER "${WRC_NORMALIZED_FILE}" WRC_LOWER_FILE)
	get_filename_component(WRC_FILE_NAME "${WRC_LOWER_FILE}" NAME)
	if(WRC_LOWER_FILE MATCHES "^automation/"
		OR WRC_LOWER_FILE MATCHES "\\.pyc?$"
		OR WRC_FILE_NAME MATCHES "^[0-9]+\\.json$"
		OR WRC_FILE_NAME MATCHES "(_test|_tests)\\.exe$"
		OR WRC_FILE_NAME MATCHES "resource_probe\\.exe$")
		message(FATAL_ERROR
			"Development-only file found in production runtime: ${WRC_NORMALIZED_FILE}")
	endif()
endforeach()

message(STATUS "Production runtime verification passed: ${WRC_RUNTIME_DIRECTORY}")

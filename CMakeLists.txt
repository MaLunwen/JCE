cmake_minimum_required(VERSION 3.20)
project(JCE C)

# Export compile database for tooling (cppcheck/clangd/etc.).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Static analysis target: uses the generated compile_commands.json.
option(JCE_ENABLE_CPPCHECK "Enable cppcheck target" ON)
if(JCE_ENABLE_CPPCHECK)
	find_program(CPPCHECK_EXECUTABLE NAMES cppcheck HINTS "D:/CMake/Cppcheck")
	if(CPPCHECK_EXECUTABLE)
		set(JCE_CPPCHECK_REPORT_DIR "${CMAKE_BINARY_DIR}/reports")
		set(JCE_CPPCHECK_TEXT_REPORT "${JCE_CPPCHECK_REPORT_DIR}/cppcheck-report.txt")
		set(JCE_CPPCHECK_XML_REPORT  "${JCE_CPPCHECK_REPORT_DIR}/cppcheck-report.xml")

		add_custom_target(cppcheck ALL
			COMMAND "${CMAKE_COMMAND}" -E make_directory "${JCE_CPPCHECK_REPORT_DIR}"
			COMMAND "${CPPCHECK_EXECUTABLE}"
				--project=${CMAKE_BINARY_DIR}/compile_commands.json
				--relative-paths=${CMAKE_SOURCE_DIR}
				--enable=warning,style,performance,portability
				--inconclusive
				--inline-suppr
				--suppress=missingIncludeSystem

				--suppress=*:*/.conan2/*
				--file-filter=${CMAKE_SOURCE_DIR}/src/*
				--std=c99
				--template={file}:{line}:{severity}:{id}:{message}
				--output-file=${JCE_CPPCHECK_TEXT_REPORT}
			COMMAND "${CPPCHECK_EXECUTABLE}"
				--project=${CMAKE_BINARY_DIR}/compile_commands.json
				--relative-paths=${CMAKE_SOURCE_DIR}
				--enable=warning,style,performance,portability
				--inconclusive
				--inline-suppr
				--suppress=missingIncludeSystem

				--suppress=*:*/.conan2/*
				--file-filter=${CMAKE_SOURCE_DIR}/src/*
				--std=c99
				--xml
				--xml-version=2
				--output-file=${JCE_CPPCHECK_XML_REPORT}
			COMMAND "${CPPCHECK_EXECUTABLE}"
				--project=${CMAKE_BINARY_DIR}/compile_commands.json
				--relative-paths=${CMAKE_SOURCE_DIR}
				--enable=warning,style,performance,portability
				--inconclusive
				--inline-suppr
				--suppress=missingIncludeSystem

				--suppress=*:*/.conan2/*
				--file-filter=${CMAKE_SOURCE_DIR}/src/*
				--std=c99
				--quiet
			WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
			COMMENT "Running cppcheck and generating filtered reports"
			VERBATIM)
	else()
		message(STATUS "cppcheck not found. Install it or add it to PATH to use target 'cppcheck'.")
	endif()
endif()

add_subdirectory(src)

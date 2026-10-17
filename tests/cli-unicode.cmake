if(NOT DEFINED CLI OR NOT DEFINED TEST_DIRECTORY)
    message(FATAL_ERROR "CLI and TEST_DIRECTORY are required")
endif()

# UTF-8 bytes for micro sign, Han character and a supplementary-plane character.
# Creating and invoking the real path exercises the CRT, not injected UTF-8 argv.
string(ASCII 194 181 230 188 162 240 159 154 128 unicode)
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef unique)
file(MAKE_DIRECTORY "${TEST_DIRECTORY}")
set(input "${TEST_DIRECTORY}/cli-${unique}-${unicode} space.html")
file(WRITE "${input}" "<p>Native UTF-8 argv converted.</p>")
execute_process(COMMAND "${CLI}" convert --file "${input}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 15)
file(REMOVE "${input}")
if(NOT result STREQUAL "0" OR NOT output STREQUAL "Native UTF-8 argv converted.\n" OR NOT errors STREQUAL "")
    message(FATAL_ERROR "Native Unicode file conversion failed: exit=${result}; stdout=${output}; stderr=${errors}")
endif()

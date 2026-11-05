# 生成 version.c：固件版本，设置页下方显示。取 git 短提交号，工作区有未提交改动时加「+」。
# 内容不变时不改写文件，避免无谓的重新链接。
execute_process(COMMAND git -C ${SRC} rev-parse --short HEAD OUTPUT_VARIABLE rev OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
execute_process(COMMAND git -C ${SRC} status --porcelain OUTPUT_VARIABLE dirty OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
if(rev STREQUAL "")
    set(rev "-")
elseif(NOT dirty STREQUAL "")
    string(APPEND rev "+")
endif()
set(text "const char kFirmwareVersion[] = \"${rev}\";\n")
if(EXISTS ${OUT})
    file(READ ${OUT} old)
endif()
if(NOT "${old}" STREQUAL "${text}")
    file(WRITE ${OUT} "${text}")
endif()

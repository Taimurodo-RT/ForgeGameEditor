# Puts a Python of its own next to the editor, with the converters'
# libraries: run as `cmake -DDIR=... -DREQUIREMENTS=... -DSTAMP=... -P`.
# Windows only: the official "embeddable" Python, which needs nothing
# installed on the computer.
set(VERSION 3.12.10)
string(REGEX MATCH "^[0-9]+\\.[0-9]+" SHORT ${VERSION})
string(REPLACE "." "" TAG ${SHORT})

if(NOT EXISTS "${DIR}/python.exe")
  message(STATUS "Python ${VERSION} для конвертеров: скачиваю")
  set(zip "${DIR}.zip")
  file(DOWNLOAD "https://www.python.org/ftp/python/${VERSION}/python-${VERSION}-embed-amd64.zip" "${zip}"
       STATUS st TLS_VERIFY ON)
  list(GET st 0 code)
  if(NOT code EQUAL 0)
    message(FATAL_ERROR "Python не скачался: ${st}")
  endif()
  file(MAKE_DIRECTORY "${DIR}")
  file(ARCHIVE_EXTRACT INPUT "${zip}" DESTINATION "${DIR}")
  file(REMOVE "${zip}")
  # The embeddable Python ignores site-packages until "import site" is on.
  file(READ "${DIR}/python${TAG}._pth" pth)
  string(REPLACE "#import site" "import site" pth "${pth}")
  file(WRITE "${DIR}/python${TAG}._pth" "${pth}Lib\\site-packages\n")
endif()

if(NOT EXISTS "${DIR}/Scripts/pip.exe")
  file(DOWNLOAD "https://bootstrap.pypa.io/get-pip.py" "${DIR}/get-pip.py" STATUS st TLS_VERIFY ON)
  list(GET st 0 code)
  if(NOT code EQUAL 0)
    message(FATAL_ERROR "pip не скачался: ${st}")
  endif()
  execute_process(COMMAND "${DIR}/python.exe" "${DIR}/get-pip.py" --no-warn-script-location RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "pip не поставился")
  endif()
endif()

execute_process(COMMAND "${DIR}/python.exe" -m pip install --disable-pip-version-check --no-warn-script-location
                        -r "${REQUIREMENTS}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "библиотеки конвертеров не поставились")
endif()
file(TOUCH "${STAMP}")

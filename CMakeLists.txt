cmake_minimum_required(VERSION 3.16)
project(kysearch VERSION 0.2.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ============ 核心引擎（不依赖 Qt，可单独用于守护进程/CLI） ============
# KYSEARCH_MINIMAL：交叉编译等场景，禁用可选依赖（RE2 / SQLite），只保留核心
option(KYSEARCH_MINIMAL "禁用可选依赖（RE2/SQLite/Qt GUI）" OFF)
find_package(PkgConfig QUIET)
set(KYSEARCH_RE2_FOUND FALSE)
set(KYSEARCH_SQLITE_FOUND FALSE)
if(PkgConfig_FOUND AND NOT KYSEARCH_MINIMAL)
  pkg_check_modules(RE2 IMPORTED_TARGET re2)
  if(RE2_FOUND)
    set(KYSEARCH_RE2_FOUND TRUE)
  endif()
  pkg_check_modules(SQLITE IMPORTED_TARGET sqlite3)
  if(SQLITE_FOUND)
    set(KYSEARCH_SQLITE_FOUND TRUE)
  endif()
  pkg_check_modules(DBUS dbus-1)
  if(DBUS_FOUND)
    set(KYSEARCH_DBUS_FOUND TRUE)
  endif()
endif()
# 无 pkg-config 环境的兜底探测（容器/精简系统常缺 pkg-config，
# QUIET 失败会让 RE2/SQLite 被静默禁用——正则与全文搜索凭空消失）
if(NOT KYSEARCH_MINIMAL)
  if(NOT KYSEARCH_RE2_FOUND)
    find_path(RE2_H re2/re2.h)
    find_library(RE2_LIB NAMES re2)
    if(RE2_H AND RE2_LIB)
      set(KYSEARCH_RE2_FOUND TRUE)
      add_library(PkgConfig::RE2 UNKNOWN IMPORTED)
      set_target_properties(PkgConfig::RE2 PROPERTIES
        IMPORTED_LOCATION "${RE2_LIB}" INTERFACE_INCLUDE_DIRECTORIES "${RE2_H}")
      message(STATUS "RE2 已启用（find_library 回退，未用 pkg-config）")
    endif()
  endif()
  if(NOT KYSEARCH_SQLITE_FOUND)
    find_path(SQLITE_H sqlite3.h)
    find_library(SQLITE_LIB NAMES sqlite3)
    if(SQLITE_H AND SQLITE_LIB)
      set(KYSEARCH_SQLITE_FOUND TRUE)
      add_library(PkgConfig::SQLITE UNKNOWN IMPORTED)
      set_target_properties(PkgConfig::SQLITE PROPERTIES
        IMPORTED_LOCATION "${SQLITE_LIB}" INTERFACE_INCLUDE_DIRECTORIES "${SQLITE_H}")
      message(STATUS "SQLite 已启用（find_library 回退，未用 pkg-config）")
    endif()
  endif()
endif()

set(KYSEARCH_CORE_SRC
  src/core/index.cpp
  src/core/scanner.cpp
  src/core/monitor.cpp
  src/core/pinyin.cpp
  src/core/fsutil.cpp)
if(KYSEARCH_SQLITE_FOUND)
  list(APPEND KYSEARCH_CORE_SRC src/core/textindex.cpp)
endif()
add_library(kysearch_core STATIC ${KYSEARCH_CORE_SRC})
target_include_directories(kysearch_core PUBLIC src)
# scanner/monitor 用 std::thread：老 glibc（focal/麒麟 2.31）需显式 -lpthread，
# 新 glibc（≥2.34）已并入 libc——统一 Threads::Threads 两边兼容
find_package(Threads REQUIRED)
target_link_libraries(kysearch_core PUBLIC Threads::Threads)
if(KYSEARCH_RE2_FOUND)
  target_compile_definitions(kysearch_core PUBLIC KYSEARCH_HAVE_RE2)
  target_link_libraries(kysearch_core PUBLIC PkgConfig::RE2)
  message(STATUS "RE2 已启用：正则搜索可用")
else()
  message(WARNING "未找到 libre2-dev，正则搜索将不可用")
endif()
if(KYSEARCH_SQLITE_FOUND)
  target_compile_definitions(kysearch_core PUBLIC KYSEARCH_HAVE_SQLITE)
  target_link_libraries(kysearch_core PUBLIC PkgConfig::SQLITE)
  message(STATUS "SQLite 已启用：全文内容搜索可用")
else()
  message(WARNING "未找到 libsqlite3-dev，全文内容搜索将不可用")
endif()

# ============ 守护进程 ============
add_executable(kysearchd src/daemon/main.cpp)
target_link_libraries(kysearchd PRIVATE kysearch_core)
if(KYSEARCH_DBUS_FOUND)
  target_compile_definitions(kysearchd PRIVATE KYSEARCH_HAVE_DBUS)
  target_include_directories(kysearchd PRIVATE ${DBUS_INCLUDE_DIRS})
  target_link_libraries(kysearchd PRIVATE ${DBUS_LIBRARIES})
  message(STATUS "D-Bus 已启用：org.kysearch.Daemon 服务接口可用")
endif()

# ============ CLI ============
add_executable(kysearch-cli src/cli/main.cpp)
target_compile_definitions(kysearch-cli PRIVATE KYSEARCH_VERSION_STR="0.2.1")
target_link_libraries(kysearch-cli PRIVATE kysearch_core)

# ============ GUI（无 Qt 环境时自动跳过，只构建 CLI） ============
if(NOT KYSEARCH_MINIMAL)
  find_package(Qt5 COMPONENTS Widgets QUIET)
endif()
if(Qt5_FOUND)
  find_package(Qt5 COMPONENTS Network QUIET)
  find_package(Qt5 COMPONENTS DBus QUIET)
  find_package(X11 QUIET)
  add_executable(kysearch-gui
    src/gui/main.cpp
    src/gui/mainwindow.cpp
    src/gui/resultmodel.cpp
    src/gui/hotkey.cpp
    src/gui/mainwindow.h
    src/gui/resultmodel.h
    src/gui/hotkey.h)
  target_link_libraries(kysearch-gui PRIVATE kysearch_core Qt5::Widgets)
  if(Qt5Network_FOUND)
    target_link_libraries(kysearch-gui PRIVATE Qt5::Network)
  endif()
  if(Qt5DBus_FOUND)
    target_link_libraries(kysearch-gui PRIVATE Qt5::DBus)
    target_compile_definitions(kysearch-gui PRIVATE KYSEARCH_HAVE_QTDBUS)
    message(STATUS "QtDBus 已启用：打开文件夹可定位选中文件（FileManager1.ShowItems）")
  endif()
  if(X11_FOUND)
    target_link_libraries(kysearch-gui PRIVATE X11::X11)
    message(STATUS "X11 已启用：全局热键（Ctrl+Alt+K）可用")
  else()
    message(WARNING "未找到 libx11-dev，全局热键将不可用")
  endif()
else()
  message(WARNING "未找到 Qt5，跳过 kysearch-gui（安装 qtbase5-dev 后重新 cmake）")
endif()

# ============ 测试 ============
enable_testing()
add_executable(kysearch-test tests/test_core.cpp)
target_link_libraries(kysearch-test PRIVATE kysearch_core)
add_test(NAME core COMMAND kysearch-test)

add_executable(FEXBash FEXBash.cpp)
target_include_directories(FEXBash PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source/)

target_link_libraries(FEXBash
  PRIVATE
    FEXCore
    Common
    JemallocLibs
)

if (CMAKE_BUILD_TYPE MATCHES "RELEASE")
  target_link_options(FEXBash
    PRIVATE
      "LINKER:--gc-sections"
      "LINKER:--strip-all"
      "LINKER:--as-needed"
  )
endif()

install(TARGETS FEXBash
  RUNTIME
    DESTINATION bin
    COMPONENT runtime
)

set(CMAKE_AUTOMOC ON)

add_executable(sensord-dashboard
  main.cpp
  main_window.cpp
  main_window.hpp
  plot_widget.cpp
  plot_widget.hpp
  rpc_client.cpp
  rpc_client.hpp
)
target_compile_options(sensord-dashboard PRIVATE ${SENSORD_WARNINGS})
target_link_libraries(sensord-dashboard PRIVATE sensord_proto Qt6::Widgets)
if(APPLE)
  set_target_properties(sensord-dashboard PROPERTIES MACOSX_BUNDLE OFF)
endif()

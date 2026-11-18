# The development tools another MGSV add-on can build on, in a dev build alone (README.md here).
add_library(devkit STATIC src/devkit/log.cpp src/devkit/host.cpp src/devkit/save_redirect.cpp src/devkit/game_lua.cpp src/devkit/travel.cpp src/devkit/cutscenes.cpp)
target_include_directories(devkit PUBLIC src)
target_link_libraries(devkit PUBLIC minhook)
target_compile_features(devkit PUBLIC cxx_std_20)
target_compile_definitions(devkit PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(devkit PRIVATE /W4 /WX /permissive- /utf-8)

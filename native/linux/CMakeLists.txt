# Linux native build of the multiplayer libraries, for the Steam Linux build of
# Transport Fever 2 (35924).
#
#   cmake -S native/linux -B native/linux/out -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build native/linux/out
#
# libstdc++ and libgcc are linked statically: the game runs inside the Steam
# Linux Runtime, whose C++ runtime is older than the one we build against, and
# our libraries must not change which one the game gets.
cmake_minimum_required(VERSION 3.18)
project(tpf2mp_linux CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# -Wno-missing-field-initializers: Vulkan structs are initialised the C way,
# `VkFooCreateInfo ci = { VK_STRUCTURE_TYPE_FOO }`, which zeroes the rest.
add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
# --exclude-libs keeps the static runtime's symbols (std::string members,
# __cxa_* exception handling) out of our dynamic symbol tables. The loader is in
# LD_PRELOAD, where every exported symbol takes precedence over the game's own
# libstdc++ for the whole process; without this it exported ~830 of them.
add_link_options(-static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL -Wl,--no-undefined -Wl,-z,now)

find_package(Threads REQUIRED)
# The UCRT math models must execute exactly the IEEE operations ucrtbase does.
set_source_files_properties(src/windows_ucrt_math_linux.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off;-fno-fast-math")

# The loader: LD_PRELOAD target, pulls in the rest.
add_library(tpf2mp_boot SHARED src/boot.cpp src/lua_fixed_format_linux.cpp
            src/destination_rng_linux.cpp src/windows_uniform_int_linux.cpp
            src/windows_person_seed_linux.cpp src/windows_person_cost_linux.cpp
            src/person_cost_linux.cpp src/float_rng_linux.cpp src/town_seed_linux.cpp src/sim_seed_linux.cpp src/engine_parity_linux.cpp src/windows_msvc_random_linux.cpp
            src/libm_parity_linux.cpp src/windows_ucrt_math_linux.cpp
            src/tree_rng_linux.cpp src/animal_rng_linux.cpp src/building_order_linux.cpp
            src/resident_hash_linux.cpp src/order_canon_linux.cpp src/person_map_order_linux.cpp
            src/windows_entity_set_order_linux.cpp src/target_order_linux.cpp src/network_person_order_linux.cpp src/network_index_order_linux.cpp
            src/hook_posix.cpp)
set_target_properties(tpf2mp_boot PROPERTIES PREFIX "lib")
target_include_directories(tpf2mp_boot PRIVATE ../src src)
target_link_libraries(tpf2mp_boot PRIVATE Threads::Threads ${CMAKE_DL_LIBS})

# The bridge: identity, the capture -> UDP -> events pump, the lobby's control
# file. Shares net.h and the hook interfaces with the Windows build.
add_library(tpf2_bridge_mp SHARED src/bridge_linux.cpp src/net_posix.cpp src/hook_posix.cpp
            src/speedhook_linux.cpp src/setplayer_linux.cpp ../src/steam_tunnel.cpp)
set_target_properties(tpf2_bridge_mp PROPERTIES PREFIX "")
target_include_directories(tpf2_bridge_mp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(tpf2_bridge_mp PRIVATE Threads::Threads ${CMAKE_DL_LIBS})

# The title menu's Multiplayer entry and its panel, drawn into the game's Vulkan
# frames. SDL2's headers are needed for its event layout only; the game's own
# libSDL2 is called through dlsym.
add_library(tpf2_menu SHARED src/menu_linux.cpp src/hook_posix.cpp src/panel_linux.cpp
            src/panel_layer.cpp src/overlay_vk_linux.cpp src/overlay_gl_linux.cpp src/lobby_linux.cpp
            src/menu_game_linux.cpp src/dedicated_linux.cpp src/native_control_linux.cpp)
set_target_properties(tpf2_menu PROPERTIES PREFIX "")
target_include_directories(tpf2_menu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(tpf2_menu PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
target_compile_options(tpf2_menu PRIVATE -fno-omit-frame-pointer)

add_library(tpf2_slice SHARED src/slice/slice_core.cpp src/slice/slice_install.cpp
            src/slice/slice_core_main.cpp src/hook_posix.cpp src/slice/slice_vehicles.cpp
            src/slice/slice_lines.cpp src/slice/slice_time.cpp src/slice/slice_proposal.cpp
            src/slice/slice_construction.cpp src/slice/slice_construction_merge.cpp src/slice/slice_terrain_assets.cpp
            src/slice/slice_terrain_codec.cpp src/slice/train_order_linux.cpp src/slice/movement_linux.cpp
            src/slice/company_tint_linux.cpp src/slice/ecs_linux.cpp)
set_target_properties(tpf2_slice PROPERTIES PREFIX "")
target_include_directories(tpf2_slice PRIVATE ../src src)
# The recursion guard in slice_install.cpp compares frame addresses.
target_compile_options(tpf2_slice PRIVATE -fno-omit-frame-pointer)
target_link_libraries(tpf2_slice PRIVATE Threads::Threads ${CMAKE_DL_LIBS})

add_library(tpf2_pluginhost SHARED src/plugin/host_linux.cpp src/plugin/cfg_linux.cpp
            src/hook_posix.cpp)
set_target_properties(tpf2_pluginhost PROPERTIES PREFIX "")
target_include_directories(tpf2_pluginhost PRIVATE ../src src)
target_link_libraries(tpf2_pluginhost PRIVATE Threads::Threads ${CMAKE_DL_LIBS})

add_library(tpf2mp_sample SHARED EXCLUDE_FROM_ALL src/plugin/sample_plugin_linux.cpp)
set_target_properties(tpf2mp_sample PROPERTIES PREFIX "")
target_include_directories(tpf2mp_sample PRIVATE ../src)

add_library(tpf2_previews SHARED src/plugin/preview_plugin_linux.cpp)
set_target_properties(tpf2_previews PROPERTIES PREFIX "")
target_include_directories(tpf2_previews PRIVATE ../src src)
target_compile_options(tpf2_previews PRIVATE -fno-omit-frame-pointer)
target_link_libraries(tpf2_previews PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
target_link_options(tpf2_previews PRIVATE -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports_plugin.map)
set_property(TARGET tpf2_previews APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/exports_plugin.map)

add_library(tpf2_workshop_register SHARED src/plugin/workshop_register_linux.cpp)
set_target_properties(tpf2_workshop_register PROPERTIES PREFIX "")
target_include_directories(tpf2_workshop_register PRIVATE ../src src)
target_compile_options(tpf2_workshop_register PRIVATE -fno-omit-frame-pointer)
target_link_libraries(tpf2_workshop_register PRIVATE ${CMAKE_DL_LIBS} stdc++fs)
target_link_options(tpf2_workshop_register PRIVATE -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports_plugin.map)
set_property(TARGET tpf2_workshop_register APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/exports_plugin.map)

# Dynamic exports: the loader exports clock() and caller-gated __sprintf_chk;
# the libraries it loads export
# nothing. Visibility flags cannot do this alone -- libstdc++'s headers force
# default visibility on the std templates they instantiate in our objects.
target_link_options(tpf2mp_boot PRIVATE -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports_boot.map)
set_property(TARGET tpf2mp_boot APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/exports_boot.map)
foreach(t tpf2_bridge_mp tpf2_menu tpf2_slice tpf2_pluginhost)
  target_link_options(${t} PRIVATE -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports_none.map)
  set_property(TARGET ${t} APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/exports_none.map)
endforeach()

include(CTest)
if(BUILD_TESTING)
  add_executable(test_setplayer_entity tests/setplayer_entity_test.cpp)
  target_compile_options(test_setplayer_entity PRIVATE -UNDEBUG)
  add_test(NAME setplayer_entity COMMAND test_setplayer_entity)
  add_executable(test_workshop_catalogue tests/workshop_catalogue_test.cpp)
  target_compile_options(test_workshop_catalogue PRIVATE -UNDEBUG)
  add_test(NAME workshop_catalogue COMMAND test_workshop_catalogue)
  add_executable(test_bridge_epoch tests/bridge_epoch_test.cpp src/net_posix.cpp)
  target_include_directories(test_bridge_epoch PRIVATE ../src)
  target_compile_options(test_bridge_epoch PRIVATE -UNDEBUG -Wno-unused-function)
  target_link_libraries(test_bridge_epoch PRIVATE Threads::Threads stdc++fs)
  add_test(NAME bridge_epoch COMMAND test_bridge_epoch)
  add_executable(test_native_control_linux tests/native_control_linux_test.cpp)
  target_compile_options(test_native_control_linux PRIVATE -UNDEBUG)
  target_link_libraries(test_native_control_linux PRIVATE Threads::Threads)
  add_test(NAME native_control_linux COMMAND test_native_control_linux)
  add_executable(test_steam_messages ../../tools/steam_messages_test.cpp)
  target_compile_options(test_steam_messages PRIVATE -UNDEBUG)
  target_link_libraries(test_steam_messages PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  add_test(NAME steam_messages COMMAND test_steam_messages)
  add_executable(test_steam_tunnel tests/steam_tunnel_test.cpp)
  target_compile_options(test_steam_tunnel PRIVATE -UNDEBUG)
  target_link_options(test_steam_tunnel PRIVATE -rdynamic)
  target_link_libraries(test_steam_tunnel PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  add_test(NAME steam_tunnel COMMAND test_steam_tunnel)
  add_test(NAME steam_tunnel_messages COMMAND test_steam_tunnel messages)
  set_tests_properties(steam_tunnel steam_tunnel_messages PROPERTIES RUN_SERIAL TRUE)
  foreach(scenario epoch multipeer packet_size restart)
    add_executable(test_net_${scenario} tests/net_${scenario}_test.cpp)
    target_include_directories(test_net_${scenario} PRIVATE ../src)
    target_compile_options(test_net_${scenario} PRIVATE -UNDEBUG)
    target_link_libraries(test_net_${scenario} PRIVATE Threads::Threads)
    if(scenario STREQUAL "multipeer")
      foreach(players 2 3 10)
        add_test(NAME net_multipeer_${players} COMMAND test_net_multipeer ${players})
      endforeach()
    else()
      add_test(NAME net_${scenario} COMMAND test_net_${scenario})
    endif()
  endforeach()
  add_executable(test_windows_uniform_int tests/windows_uniform_int_test.cpp src/windows_uniform_int_linux.cpp)
  target_include_directories(test_windows_uniform_int PRIVATE src)
  target_compile_options(test_windows_uniform_int PRIVATE -UNDEBUG)
  add_test(NAME windows_uniform_int COMMAND test_windows_uniform_int)
  add_executable(test_windows_person_seed tests/windows_person_seed_test.cpp src/windows_person_seed_linux.cpp)
  target_include_directories(test_windows_person_seed PRIVATE src)
  target_compile_options(test_windows_person_seed PRIVATE -UNDEBUG)
  add_test(NAME windows_person_seed COMMAND test_windows_person_seed)
  add_executable(test_windows_person_cost tests/windows_person_cost_test.cpp src/windows_person_cost_linux.cpp)
  target_include_directories(test_windows_person_cost PRIVATE src)
  target_compile_options(test_windows_person_cost PRIVATE -UNDEBUG)
  add_test(NAME windows_person_cost COMMAND test_windows_person_cost)
  add_executable(test_person_cost tests/person_cost_test.cpp src/windows_person_cost_linux.cpp src/hook_posix.cpp)
  target_include_directories(test_person_cost PRIVATE ../src src)
  target_compile_options(test_person_cost PRIVATE -UNDEBUG)
  add_test(NAME person_cost COMMAND test_person_cost)
  add_executable(test_float_rng tests/float_rng_test.cpp)
  target_include_directories(test_float_rng PRIVATE src)
  target_compile_options(test_float_rng PRIVATE -UNDEBUG)
  add_test(NAME float_rng COMMAND test_float_rng)
  add_executable(test_town_seed tests/town_seed_test.cpp src/windows_person_seed_linux.cpp src/hook_posix.cpp)
  target_include_directories(test_town_seed PRIVATE ../src src)
  target_compile_options(test_town_seed PRIVATE -UNDEBUG)
  add_test(NAME town_seed COMMAND test_town_seed)
  add_executable(test_sim_seed tests/sim_seed_test.cpp src/windows_person_seed_linux.cpp src/hook_posix.cpp)
  target_include_directories(test_sim_seed PRIVATE ../src src)
  target_compile_options(test_sim_seed PRIVATE -UNDEBUG)
  add_test(NAME sim_seed COMMAND test_sim_seed)
  add_executable(test_tree_rng tests/tree_rng_test.cpp src/windows_uniform_int_linux.cpp)
  target_include_directories(test_tree_rng PRIVATE ../src src)
  target_compile_options(test_tree_rng PRIVATE -UNDEBUG)
  add_test(NAME tree_rng COMMAND test_tree_rng)
  add_executable(test_animal_rng tests/animal_rng_test.cpp src/windows_person_seed_linux.cpp
                 src/windows_uniform_int_linux.cpp src/hook_posix.cpp)
  target_include_directories(test_animal_rng PRIVATE ../src src)
  target_compile_options(test_animal_rng PRIVATE -UNDEBUG)
  target_link_libraries(test_animal_rng PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  add_test(NAME animal_rng COMMAND test_animal_rng)
  add_executable(test_engine_parity tests/engine_parity_test.cpp src/windows_msvc_random_linux.cpp
                 src/windows_uniform_int_linux.cpp src/hook_posix.cpp)
  target_include_directories(test_engine_parity PRIVATE ../src src tests)
  target_compile_options(test_engine_parity PRIVATE -UNDEBUG)
  target_link_libraries(test_engine_parity PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  add_test(NAME engine_parity COMMAND test_engine_parity)
  add_executable(test_libm_parity tests/libm_parity_test.cpp src/windows_ucrt_math_linux.cpp)
  target_include_directories(test_libm_parity PRIVATE ../src src tests)
  target_compile_options(test_libm_parity PRIVATE -UNDEBUG -O2)
  target_link_libraries(test_libm_parity PRIVATE ${CMAKE_DL_LIBS} m)
  add_test(NAME libm_parity COMMAND test_libm_parity)
  add_executable(test_building_order tests/building_order_test.cpp src/hook_posix.cpp)
  target_include_directories(test_building_order PRIVATE ../src src)
  target_compile_options(test_building_order PRIVATE -UNDEBUG)
  add_test(NAME building_order COMMAND test_building_order)
  add_executable(test_resident_hash tests/resident_hash_test.cpp src/hook_posix.cpp)
  target_include_directories(test_resident_hash PRIVATE ../src src)
  target_compile_options(test_resident_hash PRIVATE -UNDEBUG)
  target_link_libraries(test_resident_hash PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  add_test(NAME resident_hash COMMAND test_resident_hash $<TARGET_FILE:slice_test_foreign>)
  add_executable(test_order_canon tests/order_canon_test.cpp src/hook_posix.cpp)
  target_include_directories(test_order_canon PRIVATE ../src src)
  target_compile_options(test_order_canon PRIVATE -UNDEBUG -fno-omit-frame-pointer)
  target_link_libraries(test_order_canon PRIVATE Threads::Threads)
  add_test(NAME order_canon COMMAND test_order_canon)
  add_executable(test_family_canon ../../tools/family_canon_test.cpp)
  add_test(NAME family_canon COMMAND test_family_canon)
  add_executable(test_person_map_order tests/person_map_order_test.cpp src/hook_posix.cpp)
  target_include_directories(test_person_map_order PRIVATE ../src src)
  target_compile_options(test_person_map_order PRIVATE -UNDEBUG)
  target_link_libraries(test_person_map_order PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  add_test(NAME person_map_order COMMAND test_person_map_order)
  add_executable(test_target_order tests/target_order_test.cpp src/windows_entity_set_order_linux.cpp src/hook_posix.cpp)
  target_include_directories(test_target_order PRIVATE ../src src)
  target_compile_options(test_target_order PRIVATE -UNDEBUG)
  target_link_libraries(test_target_order PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  add_test(NAME target_order COMMAND test_target_order)
  add_executable(test_network_person_order tests/network_person_order_test.cpp src/windows_entity_set_order_linux.cpp src/hook_posix.cpp)
  target_include_directories(test_network_person_order PRIVATE ../src src)
  target_compile_options(test_network_person_order PRIVATE -UNDEBUG)
  target_link_libraries(test_network_person_order PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  add_test(NAME network_person_order COMMAND test_network_person_order)
  add_executable(test_network_index_order tests/network_index_order_test.cpp src/windows_entity_set_order_linux.cpp src/hook_posix.cpp)
  target_include_directories(test_network_index_order PRIVATE ../src src)
  target_compile_options(test_network_index_order PRIVATE -UNDEBUG)
  target_link_libraries(test_network_index_order PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  add_test(NAME network_index_order COMMAND test_network_index_order)
  add_executable(test_destination_rng tests/destination_rng_test.cpp src/windows_uniform_int_linux.cpp
                 src/windows_person_seed_linux.cpp src/hook_posix.cpp)
  target_include_directories(test_destination_rng PRIVATE ../src src)
  target_compile_options(test_destination_rng PRIVATE -UNDEBUG)
  add_test(NAME destination_rng COMMAND test_destination_rng)
  add_executable(test_lua_fixed_format tests/lua_fixed_format_test.cpp src/lua_fixed_format_linux.cpp)
  target_include_directories(test_lua_fixed_format PRIVATE src)
  target_compile_options(test_lua_fixed_format PRIVATE -UNDEBUG)
  add_test(NAME lua_fixed_format COMMAND test_lua_fixed_format
           "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/geometry_quarter_native.txt")
  add_executable(test_boot_format_passthrough tests/boot_format_passthrough.cpp)
  target_compile_options(test_boot_format_passthrough PRIVATE -UNDEBUG)
  target_link_libraries(test_boot_format_passthrough PRIVATE ${CMAKE_DL_LIBS})
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME palette_sync COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/palette_sync_test.py")
    add_test(NAME boot_format_passthrough COMMAND "${Python3_EXECUTABLE}"
             "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/linux/test_boot_format.py"
             --boot $<TARGET_FILE:tpf2mp_boot> --probe $<TARGET_FILE:test_boot_format_passthrough>)
  endif()
  add_executable(test_linux_hooks tests/hooks.cpp src/hook_posix.cpp)
  target_include_directories(test_linux_hooks PRIVATE ../src src)
  target_compile_options(test_linux_hooks PRIVATE -UNDEBUG)
  target_link_libraries(test_linux_hooks PRIVATE Threads::Threads)
  add_test(NAME linux_hooks COMMAND test_linux_hooks)
  add_executable(test_menu_slot tests/menu_slot_test.cpp src/dedicated_linux.cpp)
  target_include_directories(test_menu_slot PRIVATE ../src src)
  target_compile_options(test_menu_slot PRIVATE -UNDEBUG -fno-omit-frame-pointer)
  target_link_libraries(test_menu_slot PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  add_test(NAME menu_slot COMMAND test_menu_slot)
  add_executable(test_lobby_ready tests/lobby_ready_test.cpp)
  target_include_directories(test_lobby_ready PRIVATE ../src src)
  target_compile_options(test_lobby_ready PRIVATE -UNDEBUG -ffunction-sections -fdata-sections)
  target_link_options(test_lobby_ready PRIVATE -Wl,--gc-sections)
  target_link_libraries(test_lobby_ready PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  add_test(NAME lobby_ready COMMAND test_lobby_ready)
  add_executable(test_slice_proposal tests/slice_proposal_test.cpp src/slice/slice_proposal.cpp
                 src/slice/slice_core.cpp src/hook_posix.cpp)
  target_include_directories(test_slice_proposal PRIVATE ../src src)
  target_compile_options(test_slice_proposal PRIVATE -UNDEBUG)
  target_link_libraries(test_slice_proposal PRIVATE Threads::Threads)
  add_test(NAME slice_proposal COMMAND test_slice_proposal)
  add_executable(test_slice_cancel tests/slice_cancel_test.cpp src/slice/slice_core.cpp src/hook_posix.cpp)
  target_include_directories(test_slice_cancel PRIVATE ../src src)
  target_compile_options(test_slice_cancel PRIVATE -UNDEBUG -fno-omit-frame-pointer)
  target_link_libraries(test_slice_cancel PRIVATE Threads::Threads)
  add_test(NAME slice_cancel COMMAND test_slice_cancel)
  add_executable(test_slice_construction_terrain tests/slice_construction_terrain_test.cpp
                 src/slice/slice_core.cpp src/hook_posix.cpp src/slice/slice_construction.cpp
                 src/slice/slice_construction_merge.cpp src/slice/slice_proposal.cpp
                 src/slice/slice_terrain_codec.cpp src/slice/slice_terrain_assets.cpp)
  target_include_directories(test_slice_construction_terrain PRIVATE ../src src)
  target_compile_options(test_slice_construction_terrain PRIVATE -UNDEBUG -Wno-class-memaccess)
  target_link_libraries(test_slice_construction_terrain PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  add_test(NAME slice_construction_terrain COMMAND test_slice_construction_terrain)
  # The game's exceptions use its dynamic runtime. Keep this fixture dynamic
  # while the detour harness has the hidden static runtime of our libraries.
  add_library(slice_test_foreign MODULE ../../tools/linux/test_slice_foreign_unwind.cpp)
  set_target_properties(slice_test_foreign PROPERTIES LINK_OPTIONS ""
                        CXX_VISIBILITY_PRESET default
                        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
  add_executable(test_animal_foreign_unwind tests/animal_foreign_unwind_test.cpp
                 src/windows_person_seed_linux.cpp src/windows_uniform_int_linux.cpp src/hook_posix.cpp)
  target_include_directories(test_animal_foreign_unwind PRIVATE ../src src)
  target_compile_options(test_animal_foreign_unwind PRIVATE -UNDEBUG -Werror -fno-omit-frame-pointer)
  target_link_libraries(test_animal_foreign_unwind PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  add_test(NAME animal_foreign_unwind COMMAND test_animal_foreign_unwind $<TARGET_FILE:slice_test_foreign>)
  add_executable(test_resident_foreign_unwind tests/resident_foreign_unwind_test.cpp src/hook_posix.cpp)
  target_include_directories(test_resident_foreign_unwind PRIVATE ../src src)
  target_compile_options(test_resident_foreign_unwind PRIVATE -UNDEBUG -Werror -fno-omit-frame-pointer)
  target_link_libraries(test_resident_foreign_unwind PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  add_test(NAME resident_foreign_unwind COMMAND test_resident_foreign_unwind $<TARGET_FILE:slice_test_foreign>)
  add_library(person_map_foreign_game MODULE tests/person_map_foreign_game.cpp)
  set_target_properties(person_map_foreign_game PROPERTIES LINK_OPTIONS ""
                        CXX_VISIBILITY_PRESET default
                        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
  add_executable(test_person_map_foreign_unwind tests/person_map_foreign_unwind_test.cpp src/hook_posix.cpp)
  target_include_directories(test_person_map_foreign_unwind PRIVATE ../src src)
  target_compile_options(test_person_map_foreign_unwind PRIVATE -UNDEBUG -Werror -fno-omit-frame-pointer)
  target_link_libraries(test_person_map_foreign_unwind PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  add_test(NAME person_map_foreign_unwind COMMAND test_person_map_foreign_unwind $<TARGET_FILE:person_map_foreign_game>)
  add_executable(test_ordering_foreign_unwind tests/ordering_foreign_unwind_test.cpp
                 src/windows_entity_set_order_linux.cpp src/hook_posix.cpp)
  target_include_directories(test_ordering_foreign_unwind PRIVATE ../src src)
  target_compile_options(test_ordering_foreign_unwind PRIVATE -UNDEBUG -Werror -fno-omit-frame-pointer)
  target_link_libraries(test_ordering_foreign_unwind PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  add_test(NAME ordering_foreign_unwind COMMAND test_ordering_foreign_unwind $<TARGET_FILE:person_map_foreign_game>)
  add_executable(test_slice_commands ../../tools/linux/test_slice_commands.cpp
                 src/slice/ecs_linux.cpp)
  target_compile_options(test_slice_commands PRIVATE -UNDEBUG -Werror -fno-omit-frame-pointer)
  target_include_directories(test_slice_commands PRIVATE ../src src)
  target_link_libraries(test_slice_commands PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/slice-command-data")
  add_test(NAME slice_commands COMMAND test_slice_commands $<TARGET_FILE:slice_test_foreign>
           "${CMAKE_CURRENT_BINARY_DIR}/slice-command-data/")
  add_executable(test_preview_plugin tests/preview_plugin_test.cpp)
  target_include_directories(test_preview_plugin PRIVATE ../src src)
  target_compile_options(test_preview_plugin PRIVATE -UNDEBUG -Werror -fno-omit-frame-pointer)
  target_link_libraries(test_preview_plugin PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  add_test(NAME preview_plugin COMMAND test_preview_plugin $<TARGET_FILE:slice_test_foreign>)
  add_executable(test_dedicated_linux tests/dedicated_linux_test.cpp src/dedicated_linux.cpp)
  target_include_directories(test_dedicated_linux PRIVATE src)
  target_compile_options(test_dedicated_linux PRIVATE -UNDEBUG -Werror)
  add_test(NAME dedicated_linux COMMAND test_dedicated_linux)
  add_executable(test_speed_pacing tests/speed_pacing_test.cpp)
  target_include_directories(test_speed_pacing PRIVATE ../src src)
  target_compile_options(test_speed_pacing PRIVATE -UNDEBUG -ffunction-sections -fdata-sections)
  target_link_options(test_speed_pacing PRIVATE -Wl,--gc-sections)
  add_test(NAME speed_pacing COMMAND test_speed_pacing)
  add_executable(test_overlay_cache tests/overlay_cache_test.cpp)
  target_include_directories(test_overlay_cache PRIVATE ../src src)
  target_compile_options(test_overlay_cache PRIVATE -UNDEBUG -ffunction-sections -fdata-sections)
  target_link_options(test_overlay_cache PRIVATE -Wl,--gc-sections)
  add_test(NAME overlay_cache COMMAND test_overlay_cache)
  add_library(test_steam_persona SHARED tests/steam_persona_fake.cpp)
  set_target_properties(test_steam_persona PROPERTIES OUTPUT_NAME steam_api)
  target_compile_options(test_steam_persona PRIVATE -fvisibility=default)
  add_executable(test_panel_title tests/panel_title_test.cpp src/panel_layer.cpp)
  target_include_directories(test_panel_title PRIVATE ../src src)
  target_compile_options(test_panel_title PRIVATE -UNDEBUG -ffunction-sections -fdata-sections)
  target_link_options(test_panel_title PRIVATE -Wl,--gc-sections)
  target_link_libraries(test_panel_title PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  add_test(NAME panel_title COMMAND test_panel_title)
  add_executable(test_panel_names tests/panel_names_test.cpp)
  target_include_directories(test_panel_names PRIVATE ../src src)
  target_compile_options(test_panel_names PRIVATE -UNDEBUG -ffunction-sections -fdata-sections)
  target_link_options(test_panel_names PRIVATE -Wl,--gc-sections)
  target_link_libraries(test_panel_names PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/panel-name-data")
  add_test(NAME panel_names COMMAND test_panel_names $<TARGET_FILE:test_steam_persona>
           "${CMAKE_CURRENT_BINARY_DIR}/panel-name-data/")
  add_executable(test_menu_load tests/menu_load_test.cpp)
  target_include_directories(test_menu_load PRIVATE ../src src)
  target_compile_options(test_menu_load PRIVATE -UNDEBUG -ffunction-sections -fdata-sections)
  target_link_options(test_menu_load PRIVATE -Wl,--gc-sections)
  target_link_libraries(test_menu_load PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  add_test(NAME menu_load COMMAND test_menu_load)
  add_executable(test_native_io tests/native_io_test.cpp)
  target_include_directories(test_native_io PRIVATE ../src src)
  target_compile_options(test_native_io PRIVATE -UNDEBUG -ffunction-sections -fdata-sections)
  target_link_options(test_native_io PRIVATE -Wl,--gc-sections)
  target_link_libraries(test_native_io PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  add_test(NAME native_io COMMAND test_native_io)
  add_executable(test_roadspace ../../tools/roadspace_test.cpp)
  target_compile_options(test_roadspace PRIVATE -UNDEBUG -ffp-contract=off)
  add_test(NAME roadspace COMMAND test_roadspace)
  add_executable(test_moveorder ../../tools/moveorder_test.cpp)
  target_compile_options(test_moveorder PRIVATE -UNDEBUG)
  add_test(NAME moveorder COMMAND test_moveorder)
  add_executable(test_movement_linux tests/movement_linux_test.cpp src/slice/slice_core.cpp
                 src/slice/train_order_linux.cpp src/hook_posix.cpp)
  target_include_directories(test_movement_linux PRIVATE ../src src)
  target_compile_options(test_movement_linux PRIVATE -UNDEBUG -ffunction-sections -fdata-sections)
  target_link_options(test_movement_linux PRIVATE -Wl,--gc-sections)
  target_link_libraries(test_movement_linux PRIVATE ${CMAKE_DL_LIBS})
  add_test(NAME movement_linux COMMAND test_movement_linux $<TARGET_FILE:slice_test_foreign>)
  add_executable(test_train_order ../../tools/trainorder_test.cpp)
  target_compile_options(test_train_order PRIVATE -UNDEBUG)
  add_test(NAME train_order COMMAND test_train_order)
  add_executable(test_company_tint tests/company_tint_test.cpp src/slice/slice_core.cpp
                 src/slice/ecs_linux.cpp src/hook_posix.cpp)
  target_include_directories(test_company_tint PRIVATE ../src src)
  target_compile_options(test_company_tint PRIVATE -UNDEBUG -ffunction-sections -fdata-sections)
  target_link_options(test_company_tint PRIVATE -Wl,--gc-sections)
  target_link_libraries(test_company_tint PRIVATE ${CMAKE_DL_LIBS})
  add_test(NAME company_tint COMMAND test_company_tint)
  add_executable(test_train_order_linux tests/train_order_linux_test.cpp src/slice/slice_core.cpp)
  target_include_directories(test_train_order_linux PRIVATE ../src src)
  target_compile_options(test_train_order_linux PRIVATE -UNDEBUG -ffunction-sections -fdata-sections)
  target_link_options(test_train_order_linux PRIVATE -Wl,--gc-sections)
  add_test(NAME train_order_linux COMMAND test_train_order_linux)
  find_program(TPF2MP_LUA NAMES lua5.2 lua lua5.4 lua5.3)
  if(TPF2MP_LUA)
    add_test(NAME slice_line_wire COMMAND "${TPF2MP_LUA}"
             "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/linux/test_slice_line_wire.lua"
             "${CMAKE_CURRENT_SOURCE_DIR}/../..")
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
      add_executable(test_boot_environment tests/boot_environment_probe.cpp)
      add_test(NAME boot_environment COMMAND "${Python3_EXECUTABLE}"
               "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/linux/test_boot_environment.py"
               --boot $<TARGET_FILE:tpf2mp_boot> --probe $<TARGET_FILE:test_boot_environment>
               --lua "${TPF2MP_LUA}")
    endif()
  endif()
endif()

# Big Maps ships in the same native build and release.
enable_language(ASM)
add_subdirectory(../../bigmap/linux bigmap)

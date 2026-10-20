# Piano Tiles -- ESP-IDF component.
#
# The one app of the pianotiles firmware kit (crosspad-app.json "kits"): built into
# that image only. In any other kit it is a component with no sources, so the
# full firmware carries no Piano Tiles code -- every kB of an image is a kB of PSRAM
# (it is copied there at boot). The requirements stay the same either way:
# ESP-IDF reads them in an early pass that does not know the kit.
#
# The PC simulator does not read this file: it globs src/*.cpp (flat, as this
# file does).

set(_pt_srcs "")
set(_pt_kit "full")
if(DEFINED CROSSPAD_FW_KIT)
    set(_pt_kit "${CROSSPAD_FW_KIT}")
endif()
file(READ "${CMAKE_CURRENT_LIST_DIR}/crosspad-app.json" _pt_app_json)
string(JSON _pt_kit_count LENGTH "${_pt_app_json}" kits)
math(EXPR _pt_last "${_pt_kit_count} - 1")
foreach(_i RANGE ${_pt_last})
    string(JSON _k GET "${_pt_app_json}" kits ${_i})
    if(_k STREQUAL _pt_kit)
        file(GLOB _pt_srcs "${CMAKE_CURRENT_LIST_DIR}/src/*.cpp")
    endif()
endforeach()

idf_component_register(
    SRCS ${_pt_srcs}
    REQUIRES
        crosspad-core
        crosspad-gui
        lvgl
)

if(_pt_srcs)
    # WHOLE_ARCHIVE keeps the REGISTER_APP-generated _register_PianoTiles_app()
    # alive through linking -- main calls it via AppRegistry_InitAll().
    set_property(TARGET ${COMPONENT_LIB} PROPERTY WHOLE_ARCHIVE ON)
endif()

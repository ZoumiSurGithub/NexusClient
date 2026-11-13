# Integre offsets.ini dans la DLL sous forme obfusquee (XOR), pour que les signatures
# ne soient pas livrees en clair aux joueurs. Genere generated/embedded_offsets.h.
# OFFSETS_FILE est fourni par le CMakeLists principal.
set(_key 0x4E 0x65 0x78 0x75 0x73 0x21 0x5A 0x6D)   # "Nexus!Zm"
list(LENGTH _key _klen)
set(_out "")
set(_len 0)

if(EXISTS "${OFFSETS_FILE}")
  file(READ "${OFFSETS_FILE}" _hex HEX)
  string(REGEX MATCHALL ".." _bytes "${_hex}")
  set(_i 0)
  foreach(_b ${_bytes})
    math(EXPR _ki "${_i} % ${_klen}")
    list(GET _key ${_ki} _kv)
    math(EXPR _x "0x${_b} ^ ${_kv}")
    string(APPEND _out "${_x},")
    math(EXPR _i "${_i} + 1")
  endforeach()
  list(LENGTH _bytes _len)
  message(STATUS "Nexus : offsets.ini integre dans la DLL (${_len} octets obfusques)")
else()
  message(STATUS "Nexus : pas d'offsets.ini a integrer (les joueurs devront en fournir un)")
endif()

configure_file(${CMAKE_SOURCE_DIR}/cmake/embedded_offsets.h.in
               ${CMAKE_BINARY_DIR}/generated/embedded_offsets.h @ONLY)

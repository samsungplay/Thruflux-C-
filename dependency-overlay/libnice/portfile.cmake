vcpkg_from_gitlab(
    GITLAB_URL https://gitlab.freedesktop.org
    OUT_SOURCE_PATH SOURCE_PATH
    REPO libnice/libnice
    REF 0.1.22
    SHA512 545c759a827e039d0aed262a4ec31b17610f7e67d93389c939763ed3d99530a4a6c3d13864ff05a2011fb3c3847ce3380a988e554de0f92b1348ebb76f5e3da4
    HEAD_REF master
)

vcpkg_replace_string(
	"${SOURCE_PATH}/meson.build"
	"werror = get_option('werror')"
	"werror = false"
)

# Strip /Zi (and -Zi) from any env flags passed into Meson by vcpkg/toolchain
foreach(_var IN ITEMS CFLAGS CXXFLAGS CPPFLAGS)
    if(DEFINED ENV{${_var}})
        set(_tmp "$ENV{${_var}}")
        string(REGEX REPLACE "(^|[ \t])[-/]Zi([ \t]|$)" " " _tmp "${_tmp}")
        string(REGEX REPLACE "[ \t]+" " " _tmp "${_tmp}")
        string(STRIP "${_tmp}" _tmp)
        set(ENV{${_var}} "${_tmp}")
    endif()
endforeach()



vcpkg_configure_meson(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -Dbuildtype=plain
        -Ddebug=false
        -Dwerror=false
        -Dwarning_level=1
        -Dgtk_doc=disabled
        -Dintrospection=disabled
        -Dtests=disabled
        -Dexamples=disabled
        -Dgstreamer=disabled
        -Dcrypto-library=openssl
        # Use comma-separated arrays for Meson flags
        "-Dc_args=['/DWIN32_LEAN_AND_MEAN', '/Zc:preprocessor', '/DNOCRYPT']"
        "-Dcpp_args=['/DWIN32_LEAN_AND_MEAN', '/Zc:preprocessor', '/DNOCRYPT']"
    OPTIONS_DEBUG
        "-Dc_args=['/DWIN32_LEAN_AND_MEAN', '/Zc:preprocessor', '/DNOCRYPT', '/Z7']"
        "-Dcpp_args=['/DWIN32_LEAN_AND_MEAN', '/Zc:preprocessor', '/DNOCRYPT', '/Z7']"
    OPTIONS_RELEASE
        # DO NOT leave these empty; repeat the core flags here
        "-Dc_args=['/DWIN32_LEAN_AND_MEAN', '/Zc:preprocessor', '/DNOCRYPT']"
        "-Dcpp_args=['/DWIN32_LEAN_AND_MEAN', '/Zc:preprocessor', '/DNOCRYPT']"
    ADDITIONAL_BINARIES
        glib-genmarshal='${CURRENT_HOST_INSTALLED_DIR}/tools/glib/glib-genmarshal'
        glib-mkenums='${CURRENT_HOST_INSTALLED_DIR}/tools/glib/glib-mkenums'
)



# Could be features:
# option('gupnp', type: 'feature', value: 'auto',
  # description: 'Enable or disable GUPnP IGD support')
# option('ignored-network-interface-prefix', type: 'array', value: ['docker', 'veth', 'virbr', 'vnet'],
  # description: 'Ignore network interfaces whose name starts with a string from this list in the ICE connection check algorithm. For example, "virbr" to ignore virtual bridge interfaces added by virtd, which do not help in finding connectivity.')
# option('crypto-library', type: 'combo', choices : ['auto', 'gnutls', 'openssl'], value : 'auto')

vcpkg_install_meson()

vcpkg_copy_pdbs()
vcpkg_copy_tools(TOOL_NAMES stunbdc stund AUTO_CLEAN)
vcpkg_fixup_pkgconfig()

vcpkg_install_copyright(
  FILE_LIST
    "${SOURCE_PATH}/COPYING"
    "${SOURCE_PATH}/COPYING.LGPL"
    "${SOURCE_PATH}/COPYING.MPL"
)

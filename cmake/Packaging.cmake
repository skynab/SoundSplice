# The Windows installer, and what it installs.
#
# Included from the top-level CMakeLists once the app and the tools exist.
#
#   cmake --build build --config Release
#   cpack --config build/CPackConfig.cmake -C Release -B build
#   powershell scripts/build-msi-bundle.ps1 -BuildDir build
#
# produces build/SoundSplice-<version>-win64.msi and, wrapped around it,
# build/SoundSplice-<version>-win64.exe - the file that is shipped. Both need
# the WiX Toolset v3 (candle.exe and light.exe) on PATH or under %WIX%.
#
# Windows only. Linux and macOS are still packaged by hand in the CI workflow,
# as a tarball and a disk image, and nothing here changes what they get.
#
# Everything the app needs is linked statically - JUCE, LAME, the codecs, Lua,
# RNNoise, whisper.cpp - so the install is the two executables, the project
# icon, and the MSVC runtime from InstallRequiredSystemLibraries below.

if(NOT WIN32)
    return()
endif()

include(GNUInstallDirs)

install(TARGETS SoundSplice RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT runtime)
# Beside the app on purpose: soundsplice-cli renders by running SoundSplice.exe,
# and looks for it in its own folder first (tools/cli/main.cpp, findApp).
if(TARGET soundsplice_cli)
    install(TARGETS soundsplice_cli RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT runtime)
endif()

# What a .splice file looks like in Explorer. A separate file rather than a
# second icon compiled into SoundSplice.exe, because JUCE writes the app's
# resource script itself and has room in it for exactly one icon.
install(FILES "${PROJECT_SOURCE_DIR}/resources/branding/SoundSplice-Project.ico"
    DESTINATION "${CMAKE_INSTALL_BINDIR}"
    COMPONENT runtime)

# The Visual C++ runtime. A machine that has never had Visual Studio on it does
# not carry vcruntime140.dll, and both executables need it. The UCRT is left
# out: it has been part of Windows since 10.
set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION "${CMAKE_INSTALL_BINDIR}")
set(CMAKE_INSTALL_SYSTEM_RUNTIME_COMPONENT runtime)
set(CMAKE_INSTALL_UCRT_LIBRARIES OFF)
include(InstallRequiredSystemLibraries)

# WiX's licence page wants .txt or .rtf, and LICENSE has no extension.
configure_file("${PROJECT_SOURCE_DIR}/LICENSE" "${CMAKE_BINARY_DIR}/LICENSE.txt" COPYONLY)
install(FILES "${CMAKE_BINARY_DIR}/LICENSE.txt" "${PROJECT_SOURCE_DIR}/README.md"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/SoundSplice"
    COMPONENT runtime)

set(CPACK_PACKAGE_NAME "SoundSplice")
set(CPACK_PACKAGE_VENDOR "Anthony Lazzaro")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A focused audio editor")
set(CPACK_PACKAGE_HOMEPAGE_URL "${PROJECT_HOMEPAGE_URL}")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_BINARY_DIR}/LICENSE.txt")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "SoundSplice")
set(CPACK_PACKAGE_FILE_NAME "SoundSplice-${PROJECT_VERSION}-win64")
set(CPACK_VERBATIM_VARIABLES ON)
set(CPACK_GENERATOR WIX)

# --- The MSI -------------------------------------------------------------------
# Stable for the life of the product, and the one value here that must never
# change: it is how the installer recognises an older SoundSplice and replaces
# it. A new GUID turns every upgrade into a second copy beside the first.
set(CPACK_WIX_UPGRADE_GUID "0DEBE0A4-9906-40EA-B914-FDF672B38DE7")

# Stated rather than left to CPack's default of none, under which no shortcut
# or registry key can say which hive it belongs to and WiX's validator refuses
# the package (ICE57/ICE90). perMachine: Program Files, the all-users Start Menu
# and Desktop, and an elevated install that can write HKLM.
set(CPACK_WIX_INSTALL_SCOPE "perMachine")

set(CPACK_WIX_PROGRAM_MENU_FOLDER "SoundSplice")
set(CPACK_WIX_ROOT_FEATURE_TITLE "SoundSplice")
# The Start Menu entry, pointing at the app rather than the command-line tool.
set(CPACK_PACKAGE_EXECUTABLES "SoundSplice" "SoundSplice")

# The desktop shortcut and the .splice association, which CPack has no
# variables for. See the file.
set(CPACK_WIX_PATCH_FILE "${PROJECT_SOURCE_DIR}/cmake/wix-patch.xml")

# The installer's own face. The product icon is what Add/Remove Programs lists
# the entry with. The shortcuts take theirs from SoundSplice.exe, which carries
# the application mark (src/app/CMakeLists.txt, ICON_BIG).
#
# The two bitmaps are the WixUI stock sizes, 493x58 and 493x312; anything else
# is stretched to fit. The dialog's art keeps to the left 164 pixels, where
# WixUI leaves room for it beside the welcome text.
set(CPACK_WIX_PRODUCT_ICON "${PROJECT_SOURCE_DIR}/resources/branding/SoundSplice-Installer.ico")
set(CPACK_WIX_UI_BANNER "${PROJECT_SOURCE_DIR}/resources/branding/installer-banner.bmp")
set(CPACK_WIX_UI_DIALOG "${PROJECT_SOURCE_DIR}/resources/branding/installer-dialog.bmp")
set(CPACK_WIX_PROPERTY_ARPURLINFOABOUT "${PROJECT_HOMEPAGE_URL}")

# One component, and it is the product: the feature tree says "SoundSplice"
# and offers no choice, rather than a "runtime" checkbox nothing works without.
set(CPACK_COMPONENTS_ALL runtime)
set(CPACK_COMPONENT_RUNTIME_DISPLAY_NAME "SoundSplice")
set(CPACK_COMPONENT_RUNTIME_REQUIRED ON)

# --- The .exe around the MSI -----------------------------------------------------
# An .msi cannot carry its own icon - see cmake/bundle.wxs.in - so the file
# people download is a Burn bundle wrapping it. Written here, built by
# scripts/build-msi-bundle.ps1 once cpack has made the MSI.
set(SOUNDSPLICE_BUNDLE_NAME "${CPACK_PACKAGE_NAME}")
set(SOUNDSPLICE_BUNDLE_VERSION "${CPACK_PACKAGE_VERSION}")
set(SOUNDSPLICE_BUNDLE_VENDOR "${CPACK_PACKAGE_VENDOR}")
set(SOUNDSPLICE_BUNDLE_ICON "${PROJECT_SOURCE_DIR}/resources/branding/SoundSplice-Installer.ico")
set(SOUNDSPLICE_BUNDLE_LOGO "${PROJECT_SOURCE_DIR}/resources/branding/SoundSplice-Installer-64.png")
set(SOUNDSPLICE_BUNDLE_LICENSE_URL "${PROJECT_HOMEPAGE_URL}/blob/dev/LICENSE")
# Absolute, because light.exe resolves a relative SourceFile against its own
# working directory.
set(SOUNDSPLICE_BUNDLE_MSI "${CMAKE_BINARY_DIR}/${CPACK_PACKAGE_FILE_NAME}.msi")
configure_file("${PROJECT_SOURCE_DIR}/cmake/bundle.wxs.in" "${CMAKE_BINARY_DIR}/bundle.wxs" @ONLY)

include(CPack)

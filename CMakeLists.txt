cmake_minimum_required(VERSION 3.25)

# --- vcpkg ツールチェーンの解決 -------------------------------------------
# VCPKG_ROOT が設定されていればそれを使い、なければ C:/vcpkg にフォールバックする。
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VCPKG_ROOT})
        set(hm_vcpkgRoot "$ENV{VCPKG_ROOT}")
    else()
        set(hm_vcpkgRoot "C:/vcpkg")
    endif()
    set(hm_vcpkgToolchain "${hm_vcpkgRoot}/scripts/buildsystems/vcpkg.cmake")
    if(EXISTS "${hm_vcpkgToolchain}")
        set(CMAKE_TOOLCHAIN_FILE "${hm_vcpkgToolchain}" CACHE STRING "vcpkg toolchain file")
        message(STATUS "vcpkg toolchain: ${hm_vcpkgToolchain}")
    else()
        message(FATAL_ERROR "vcpkg のツールチェーンが見つかりません: ${hm_vcpkgToolchain}")
    endif()
endif()

project(terrain_graph VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "本プロジェクトは Windows 専用です。")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# マルチコンフィグジェネレータでも出力先を揃える。
foreach(cfg Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${cfg}" cfgUpper)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfgUpper} "${CMAKE_BINARY_DIR}/bin/${cfg}")
endforeach()

# --- Windows SDK の d3d12.lib を先に解決する -------------------------------
# Visual Studio ジェネレータでは Windows SDK の lib パスが find_library の探索対象に
# 入らないため、directx12-agility の config が失敗する。先にキャッシュへ入れておく。
if(NOT D3D12_LIB)
    if(DEFINED CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
        set(hm_sdkVersion "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
    else()
        set(hm_sdkVersion "${CMAKE_SYSTEM_VERSION}")
    endif()

    # Windows SDK のインストール先をレジストリから引く（無ければ既定パスにフォールバック）。
    cmake_host_system_information(RESULT hm_kitsRoot
        QUERY WINDOWS_REGISTRY "HKLM/SOFTWARE/Microsoft/Windows Kits/Installed Roots"
        VALUE "KitsRoot10"
        VIEW 32
        ERROR_VARIABLE hm_kitsRootError)
    if(NOT hm_kitsRoot)
        set(hm_kitsRoot "C:/Program Files (x86)/Windows Kits/10")
    endif()
    file(TO_CMAKE_PATH "${hm_kitsRoot}" hm_kitsRoot)

    find_library(D3D12_LIB
        NAMES d3d12
        HINTS
            "${hm_kitsRoot}/Lib/${hm_sdkVersion}/um/x64"
            "$ENV{WindowsSdkDir}Lib/${hm_sdkVersion}/um/x64"
        PATHS ENV LIB
    )
    if(NOT D3D12_LIB)
        message(FATAL_ERROR "Windows SDK の d3d12.lib が見つかりません (SDK ${hm_sdkVersion})")
    endif()
    message(STATUS "d3d12.lib: ${D3D12_LIB}")
endif()

find_package(directx-headers CONFIG REQUIRED)
find_package(directx12-agility CONFIG REQUIRED)
find_package(winpixevent CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(D3D12MemoryAllocator CONFIG REQUIRED)
find_package(directx-dxc CONFIG REQUIRED)
find_package(Stb REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(tinyexr CONFIG REQUIRED)
find_package(unofficial-imgui-node-editor CONFIG REQUIRED)

set(tg_sources
    src/app/Main.cpp
    src/app/TerrainGraph.rc
    src/app/Application.cpp
    src/app/Application.h
    src/app/ApplicationDocument.cpp
    src/app/ApplicationExport.cpp
    src/app/ApplicationFileWork.cpp
    src/app/ApplicationGraphPanel.cpp
    src/app/ApplicationLayerPanel.cpp
    src/app/ApplicationMaterialPanel.cpp
    src/app/ApplicationPathEdit.cpp
    src/app/ApplicationPreviewPanels.cpp
    src/app/ApplicationSkyPanel.cpp
    src/app/ApplicationTexturePanel.cpp
    src/app/ApplicationUiHelpers.h
    src/app/ApplicationViewport.cpp
    src/app/UndoHistory.cpp
    src/app/UndoHistory.h
    src/core/Log.cpp
    src/core/Log.h
    src/graph/NodeGraph.cpp
    src/graph/NodeGraph.h
    src/graph/Path.cpp
    src/graph/PathRoute.cpp
    src/graph/Path.h
    src/compositor/MaterialEvaluator.cpp
    src/compositor/MaterialEvaluator.h
    src/compositor/MaterialLayer.h
    src/compositor/MaterialLibrary.cpp
    src/compositor/MaterialLibrary.h
    src/compositor/MaterialStack.cpp
    src/compositor/MaterialStack.h
    src/compositor/PaintMask.cpp
    src/compositor/PaintMask.h
    src/compositor/TextureLibrary.cpp
    src/compositor/TextureLibrary.h
    src/core/FileDialog.cpp
    src/core/FileDialog.h
    src/core/ColorSpace.h
    src/core/FrameLimiter.cpp
    src/core/FrameLimiter.h
    src/core/ImageIo.cpp
    src/core/ImageIo.h
    src/core/Shell.cpp
    src/core/Shell.h
    src/core/PathUtf8.h
    src/core/ResourceIds.h
    src/core/Window.cpp
    src/core/Window.h
    src/io/AppSettings.cpp
    src/io/AppSettings.h
    src/io/MaterialExport.cpp
    src/io/MaterialExport.h
    src/io/ProjectIo.cpp
    src/io/ProjectIo.h
    src/io/RecentFiles.cpp
    src/io/RecentFiles.h
    src/rhi/Common.cpp
    src/rhi/Common.h
    src/rhi/ComputeQueue.cpp
    src/rhi/ComputeQueue.h
    src/rhi/DeletionQueue.cpp
    src/rhi/DeletionQueue.h
    src/rhi/DescriptorHeap.cpp
    src/rhi/DescriptorHeap.h
    src/rhi/Device.cpp
    src/rhi/Device.h
    src/rhi/GpuResource.cpp
    src/rhi/GpuResource.h
    src/rhi/PipelineCache.cpp
    src/rhi/PipelineCache.h
    src/rhi/ShaderCompiler.cpp
    src/rhi/ShaderCompiler.h
    src/rhi/UploadRing.cpp
    src/rhi/UploadRing.h
    src/renderer/Camera.cpp
    src/renderer/Camera.h
    src/renderer/Environment.cpp
    src/renderer/Environment.h
    src/renderer/MaterialSphere.cpp
    src/renderer/MaterialSphere.h
    src/renderer/Mesh.cpp
    src/renderer/Mesh.h
    src/renderer/PreviewRenderer.cpp
    src/renderer/PreviewRenderer.h
    src/renderer/SkyLibrary.cpp
    src/renderer/SkyLibrary.h
    src/ui/ImGuiLayer.cpp
    src/ui/ImGuiLayer.h
    src/ui/Toast.cpp
    src/ui/Toast.h
    src/ui/UiStyle.cpp
    src/ui/UiStyle.h
)

# シェーダはビルド対象ではないが、VS のソリューションから編集できるようにしておく。
file(GLOB tg_shaders CONFIGURE_DEPENDS "shaders/*.hlsl" "shaders/*.hlsli")
set_source_files_properties(${tg_shaders} PROPERTIES HEADER_FILE_ONLY TRUE)
source_group("shaders" FILES ${tg_shaders})

add_executable(terrain_graph WIN32 ${tg_sources} ${tg_shaders})

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" PREFIX "src" FILES ${tg_sources})

target_include_directories(terrain_graph PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${Stb_INCLUDE_DIR}"
)

target_compile_definitions(terrain_graph PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    # PIX マーカーを有効にする。未定義だと pix3.h の呼び出しが消え、
    # Release で PIX による計測ができなくなる。
    USE_PIX
    TG_APP_VERSION="${PROJECT_VERSION}"
    # シェーダはホットリロードのためソースツリーを直接参照する。
    TG_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    # 手元のデータ置き場（スクリーンショットの既定の保存先もここ）。
    # data/ は .gitignore で外してあるので、撮ったものがリポジトリに混ざらない。
    # コミットするアセットは assets/ で、こちらとは別。
    TG_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    $<$<CONFIG:Debug>:TG_DEBUG=1>
)

target_compile_options(terrain_graph PRIVATE /W4 /permissive- /utf-8 /Zc:preprocessor)

target_link_libraries(terrain_graph PRIVATE
    Microsoft::DirectX-Headers
    Microsoft::DirectX-Guids
    Microsoft::DirectX12-Agility
    Microsoft::WinPixEventRuntime
    Microsoft::DirectXShaderCompiler
    GPUOpen::D3D12MemoryAllocator
    imgui::imgui
    unofficial::imgui-node-editor::imgui-node-editor
    nlohmann_json::nlohmann_json
    unofficial::tinyexr::tinyexr
    d3d12
    dxgi
    dxguid
    shell32
    # IFileDialog（ファイル選択ダイアログ）に必要。
    ole32
)

# --- Agility SDK の DLL を実行ファイル横の D3D12/ に配置する ---------------
add_custom_command(TARGET terrain_graph POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:terrain_graph>/D3D12"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_PROPERTY:Microsoft::DirectX12-Core,IMPORTED_LOCATION_RELEASE>"
        "$<TARGET_FILE_DIR:terrain_graph>/D3D12"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_PROPERTY:Microsoft::DirectX12-Layers,IMPORTED_LOCATION_DEBUG>"
        "$<TARGET_FILE_DIR:terrain_graph>/D3D12"
    COMMAND_EXPAND_LISTS
    COMMENT "Agility SDK の DLL を D3D12/ に配置"
)

# --- DXC の署名用 DLL を実行ファイル横に置く -------------------------------
# dxcompiler.dll はリンクしているため applocal で配置されるが、
# dxil.dll (署名) はリンクしていないので明示的にコピーする。
add_custom_command(TARGET terrain_graph POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_PROPERTY:Microsoft::DXIL,IMPORTED_LOCATION_DEBUG>"
        "$<TARGET_FILE_DIR:terrain_graph>"
    COMMAND_EXPAND_LISTS
    COMMENT "dxil.dll を配置"
)

# --- テスト ---------------------------------------------------------------
# UI の相互作用（ドラッグ、ホバー）は `--screenshot-ui` では確認できない。
# ImGui へマウスイベントを注入して確かめる。GPU もウィンドウも要らないので、
# アプリ本体とは別の実行ファイルにしてある。
#
# UiStyle は imgui にしか依存しないため、そのままテストへ組み込める。
# **テストがアプリの実コードを呼ぶこと**が重要で、同じ処理を書き写すと
# 実装だけ元に戻したときに検出できない。
enable_testing()

add_executable(terrain_graph_tests
    tests/TestMain.cpp
    tests/TestSupport.h
    tests/FrameLimiterTests.cpp
    tests/NodeGraphTests.cpp
    tests/UiInteractionTests.cpp
    tests/UndoHistoryTests.cpp
    src/app/UndoHistory.cpp
    src/app/UndoHistory.h
    src/core/FrameLimiter.cpp
    src/core/FrameLimiter.h
    src/compositor/MaterialStack.cpp
    src/compositor/MaterialStack.h
    src/graph/NodeGraph.cpp
    src/graph/NodeGraph.h
    src/graph/Path.cpp
    src/graph/Path.h
    src/ui/UiStyle.cpp
    src/ui/UiStyle.h
)

target_include_directories(terrain_graph_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(terrain_graph_tests PRIVATE /W4 /permissive- /utf-8 /Zc:preprocessor)
target_link_libraries(terrain_graph_tests PRIVATE imgui::imgui)

add_test(NAME ui-interaction COMMAND terrain_graph_tests)

set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY VS_STARTUP_PROJECT terrain_graph)
set_target_properties(terrain_graph PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:terrain_graph>"
)

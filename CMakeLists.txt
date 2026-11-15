cmake_minimum_required(VERSION 3.25)

project(PRISystemRack VERSION 1.0.0 LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "PRISystemRack は Windows 専用です")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 実行ファイルと DLL を build/bin/<構成>/ にまとめる
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# LTO は開発中は切り、配布用のリリースビルドのときだけ -DPRI_ENABLE_LTO=ON で有効にする
option(PRI_ENABLE_LTO "リンク時最適化を有効にする（リリースビルド用）" OFF)

if(MSVC)
    add_compile_options(/utf-8)
endif()

# 全ターゲット共通の設定
function(pri_configure_target target)
    target_compile_definitions(${target} PRIVATE UNICODE _UNICODE NOMINMAX)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
    # audiodg.exe に読み込まれる DLL が VC++ ランタイムの有無に左右されないよう、静的リンクにする
    set_target_properties(${target} PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    if(PRI_ENABLE_LTO)
        set_target_properties(${target} PROPERTIES INTERPROCEDURAL_OPTIMIZATION ON)
    endif()
endfunction()

#==============================================================================
# APO（audiodg.exe に読み込まれ、音声を本体アプリとやり取りする DLL）
add_library(PRISystemRackAPO SHARED
    Source/Apo/ApoGuids.h
    Source/Apo/ApoLog.cpp
    Source/Apo/ApoLog.h
    Source/Apo/DllMain.cpp
    Source/Apo/HostChannel.cpp
    Source/Apo/HostChannel.h
    Source/Apo/Module.h
    Source/Apo/PriApo.cpp
    Source/Apo/PriApo.h
    Source/Apo/PRISystemRackAPO.def
    Source/Common/RackIpc.h)

pri_configure_target(PRISystemRackAPO)
target_include_directories(PRISystemRackAPO PRIVATE Source)
# AudioBaseProcessingObjectV140 は UCRT 向けにビルドされた版（無印は古い CRT の _vsnwprintf を参照する）
target_link_libraries(PRISystemRackAPO PRIVATE AudioBaseProcessingObjectV140 audiomediatypecrt ole32 advapi32 uuid)
# V140 版は CRT を DLL で使う前提でビルドされているため、静的 CRT と組むと LNK4217 が出る。
# リンカーが静的 CRT 側で解決するので動作に問題はない
target_link_options(PRISystemRackAPO PRIVATE /IGNORE:4217)

#==============================================================================
# 本体アプリ側の部品（APO との受け渡し、再生デバイスの一覧、プロセスの音の取り込み）。JUCE 非依存
add_library(PRISystemRackHostCore STATIC
    Source/HostCore/ApoLink.cpp
    Source/HostCore/ApoLink.h
    Source/HostCore/DeviceWatcher.cpp
    Source/HostCore/DeviceWatcher.h
    Source/HostCore/Endpoints.cpp
    Source/HostCore/Endpoints.h
    Source/HostCore/PluginBinaryCheck.cpp
    Source/HostCore/PluginBinaryCheck.h
    Source/HostCore/ProcessCapture.cpp
    Source/HostCore/ProcessCapture.h
    Source/HostCore/ProcessTools.cpp
    Source/HostCore/ProcessTools.h
    Source/HostCore/Setup.cpp
    Source/HostCore/Setup.h
    Source/Common/RackIpc.h)

pri_configure_target(PRISystemRackHostCore)
target_include_directories(PRISystemRackHostCore PUBLIC Source)
target_link_libraries(PRISystemRackHostCore PUBLIC ole32 avrt advapi32 shell32 mmdevapi)

#==============================================================================
# 再生デバイスに APO を入れる・外す補助プログラム（管理者権限で動く）。本体アプリと同じフォルダに置く
add_executable(PRISystemRackSetup Source/Setup/PRISystemRackSetup.cpp Source/Common/RackIpc.h)
pri_configure_target(PRISystemRackSetup)
target_include_directories(PRISystemRackSetup PRIVATE Source)
target_link_libraries(PRISystemRackSetup PRIVATE advapi32)
target_link_options(PRISystemRackSetup PRIVATE "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
set_target_properties(PRISystemRackSetup PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/PRISystemRack_artefacts)

#==============================================================================
# JUCE に当てる修正（external/JUCE は取ってくるだけのライブラリなので、構成のたびに確かめて、まだなら当てる）。
# VST3 のプラグインを作ると、JUCE は MIDI の CC の割り当てを 16 チャンネル × 130 番の 2,080 回プラグインに問い合わせる
# （StoredMidiMapping::storeMappings）。MIDI の入力がないエフェクトにも問い合わせる。Waves の古い WaveShell（9.92 / 10.0）の
# プラグインは、問い合わせのたびに処理部と画面を作り直して描画資源を残し、GDI オブジェクトを上限の 10,000 個まで使い切る
# （2026-11-15、L316 Stereo の呼び出し経路で確認。Cubase では起きない）。MIDI の入力を持つ GTR Stomp 6 も、問い合わせのたびに
# 工場出荷のプリセットを読み直してメニューを作り、USER オブジェクトを使い続けた。
# 割り当ては、プラグインに送る MIDI の CC をパラメーターの変更に読み替えるときだけ使う（MidiEventList::hostToPluginEventList）。
# このアプリはプラグインに MIDI を送らない（渡す MidiBuffer はいつも空）ので、問い合わせない（PRI_JUCE_QUERY_MIDI_MAPPINGS=0）。
# 以前の修正（MIDI の入力があるプラグインにだけ問い合わせる。印 "PRISystemRack patch: midi mapping"）が当たっていれば置き換える
set(PRI_JUCE_VST3_IMPL "${CMAKE_CURRENT_SOURCE_DIR}/external/JUCE/modules/juce_audio_processors_headless/format_types/juce_VST3PluginFormatImpl.h")
set(PRI_JUCE_PATCH_V1 "if (midiMapping != nullptr && hasMidiInput) // PRISystemRack patch: midi mapping only for plugins with MIDI input (CMakeLists.txt)")
set(PRI_JUCE_PATCH_V2 "if (midiMapping != nullptr && PRI_JUCE_QUERY_MIDI_MAPPINGS) // PRISystemRack patch v2: no MIDI CC mapping queries (CMakeLists.txt)")
if(EXISTS "${PRI_JUCE_VST3_IMPL}")
    file(READ "${PRI_JUCE_VST3_IMPL}" PRI_JUCE_SOURCE)
    string(FIND "${PRI_JUCE_SOURCE}" "PRISystemRack patch v2" PRI_JUCE_PATCHED)
    if(PRI_JUCE_PATCHED EQUAL -1)
        string(FIND "${PRI_JUCE_SOURCE}" "${PRI_JUCE_PATCH_V1}" PRI_JUCE_V1)
        if(NOT PRI_JUCE_V1 EQUAL -1)
            string(REPLACE "${PRI_JUCE_PATCH_V1}" "${PRI_JUCE_PATCH_V2}" PRI_JUCE_SOURCE "${PRI_JUCE_SOURCE}")
        else()
            # 元の行（行末が CRLF でも LF でもよいよう、行末の手前までで探す）
            string(FIND "${PRI_JUCE_SOURCE}" "if (midiMapping != nullptr)" PRI_JUCE_ANCHOR)
            if(PRI_JUCE_ANCHOR EQUAL -1)
                message(FATAL_ERROR "JUCE が変わったため、MIDI の CC の問い合わせを止める修正を当てられません: ${PRI_JUCE_VST3_IMPL}")
            endif()
            string(REPLACE "if (midiMapping != nullptr)" "${PRI_JUCE_PATCH_V2}" PRI_JUCE_SOURCE "${PRI_JUCE_SOURCE}")
        endif()
        file(WRITE "${PRI_JUCE_VST3_IMPL}" "${PRI_JUCE_SOURCE}")
        message(STATUS "JUCE に修正を当てました（プラグインに MIDI の CC の割り当てを問い合わせない）")
    endif()
endif()
# 上の修正で使う。1 にすれば JUCE のもとの動き（問い合わせる）に戻る
add_compile_definitions(PRI_JUCE_QUERY_MIDI_MAPPINGS=0)

# 本体アプリ（JUCE 8）。JUCE は external/JUCE に clone しておく（README 参照）
add_subdirectory(external/JUCE EXCLUDE_FROM_ALL)

juce_add_gui_app(PRISystemRack
    PRODUCT_NAME "PRISystemRack"
    COMPANY_NAME "PRISystemRack"
    # アプリのアイコン（exe に埋め込む。ICON_SMALL は 16〜32 px 用の簡略版。Tools/MakeIcon.ps1 で描き直せる）
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Icons/icon-256.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Icons/icon-32.png")

target_sources(PRISystemRack PRIVATE
    Source/App/ActivityTap.h
    Source/App/AlwaysOnTop.h
    Source/App/AppLog.cpp
    Source/App/AppLog.h
    Source/App/AppRack.cpp
    Source/App/AppRack.h
    Source/App/AppRackWindow.cpp
    Source/App/AppRackWindow.h
    Source/App/ChainPresets.cpp
    Source/App/ChainPresets.h
    Source/App/ChainController.cpp
    Source/App/ChainController.h
    Source/App/ChainView.cpp
    Source/App/ChainView.h
    Source/App/CrashLog.cpp
    Source/App/CrashLog.h
    Source/App/DialogLaunch.h
    Source/App/EditorGuard.cpp
    Source/App/EditorGuard.h
    Source/App/EnableSwitch.h
    Source/App/GuiResources.cpp
    Source/App/GuiResources.h
    Source/App/Language.cpp
    Source/App/Language.h
    Source/App/Main.cpp
    Source/App/MainComponent.cpp
    Source/App/MainComponent.h
    Source/App/NumberBox.h
    Source/App/PluginActivityView.cpp
    Source/App/PluginActivityView.h
    Source/App/PluginBrowser.cpp
    Source/App/PluginBrowser.h
    Source/App/PluginGuard.cpp
    Source/App/PluginLoadDialog.cpp
    Source/App/PluginLoadDialog.h
    Source/App/PluginSafety.cpp
    Source/App/PluginSafety.h
    Source/App/PluginChain.cpp
    Source/App/PluginChain.h
    Source/App/PluginGuard.h
    Source/App/PluginScanner.cpp
    Source/App/PluginScanner.h
    Source/App/PluginTypePicker.cpp
    Source/App/PluginTypePicker.h
    Source/App/PluginWindow.h
    Source/App/PresetManager.cpp
    Source/App/PresetManager.h
    Source/App/ProcessPicker.cpp
    Source/App/ProcessPicker.h
    Source/App/ProcessingBuffer.cpp
    Source/App/ProcessingBuffer.h
    Source/App/WindowRendering.cpp
    Source/App/WindowRendering.h
    Source/App/RackEngine.cpp
    Source/App/RackEngine.h
    Source/App/RackModel.h
    Source/App/LevelMeterTap.h
    Source/App/SectionMeter.cpp
    Source/App/SectionMeter.h
    Source/App/TestToneToggles.h
    Source/App/OutputGuard.h
    Source/App/AppTray.cpp
    Source/App/AppTray.h
    Source/App/ScaledContent.h
    Source/App/TestTone.h
    Source/App/WheelSteps.h
    Source/App/Text.h
    Source/App/Theme.h
    Source/App/WindowCapture.cpp
    Source/App/WindowCapture.h)

target_compile_definitions(PRISystemRack PRIVATE
    JUCE_PLUGINHOST_VST3=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:PRISystemRack,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:PRISystemRack,JUCE_VERSION>")

# HostCore（静的 CRT）と合わせる
set_target_properties(PRISystemRack PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# 画面の訳（既定は英語。日本語はこのファイルで訳す）
juce_add_binary_data(PRISystemRackData SOURCES
    Resources/Translations/ja.txt
    Resources/Icons/icon-16.png   # 通知領域のアイコン（表示の倍率が 150% 未満）
    Resources/Icons/icon-32.png)  # 通知領域のアイコン（150% 以上）
set_target_properties(PRISystemRackData PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

target_link_libraries(PRISystemRack
    PRIVATE
        PRISystemRackData
        PRISystemRackHostCore
        juce::juce_audio_processors
        juce::juce_dsp
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# Release でも PDB を作る（落ちたときの記録 CrashLog が関数名・行を出せるように。最適化はそのまま）
if(MSVC)
    target_compile_options(PRISystemRack PRIVATE $<$<CONFIG:Release>:/Zi>)
    target_link_options(PRISystemRack PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
endif()

if(PRI_ENABLE_LTO)
    target_link_libraries(PRISystemRack PUBLIC juce::juce_recommended_lto_flags)
endif()

add_dependencies(PRISystemRack PRISystemRackSetup PRISystemRackScanner)

# VST3 ファイルの中のプラグインの一覧を調べる補助プログラム（本体が別のプロセスとして起動する）。本体と同じフォルダに置く
juce_add_console_app(PRISystemRackScanner PRODUCT_NAME "PRISystemRackScanner")
target_sources(PRISystemRackScanner PRIVATE Source/Scanner/PRISystemRackScanner.cpp Source/Scanner/PluginCheck.cpp Source/Scanner/PluginCheck.h)
target_compile_definitions(PRISystemRackScanner PRIVATE JUCE_PLUGINHOST_VST3=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(PRISystemRackScanner
    PRIVATE
        juce::juce_audio_processors
        PRISystemRackHostCore # PluginBinaryCheck（読み込むファイルの場所）
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
# Release でも PDB を作る（調査用の呼び出し経路（--probe ... stack）に関数名を出すため。最適化はそのまま）
target_compile_options(PRISystemRackScanner PRIVATE $<$<CONFIG:Release>:/Zi>)
target_link_options(PRISystemRackScanner PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
set_target_properties(PRISystemRackScanner PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/PRISystemRack_artefacts)

#==============================================================================
# 確認用: わざと落ちるテスト用 VST3 と、それを PluginGuard を通して呼ぶコンソールツール
juce_add_plugin(PRICrashTest
    PRODUCT_NAME "PRICrashTest"
    COMPANY_NAME "PRISystemRack"
    PLUGIN_MANUFACTURER_CODE Prsr
    PLUGIN_CODE Pcrt
    FORMATS VST3
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(PRICrashTest PRIVATE Tools/CrashTestPlugin/CrashTestPlugin.cpp)
target_compile_definitions(PRICrashTest PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(PRICrashTest
    PRIVATE
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

juce_add_console_app(GuardTest PRODUCT_NAME "GuardTest")
target_sources(GuardTest PRIVATE Tools/GuardTest/GuardTest.cpp Source/App/PluginGuard.cpp)
target_include_directories(GuardTest PRIVATE Source)
target_compile_definitions(GuardTest PRIVATE JUCE_PLUGINHOST_VST3=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(GuardTest
    PRIVATE
        juce::juce_audio_processors
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# 確認用: 無音時の休止を、本物のプラグインと作った信号で確かめる（音声デバイスは使わない）
juce_add_console_app(RestTest PRODUCT_NAME "RestTest")
# AppLog と PluginBinaryCheck は PluginChain / PluginScanner が使う。HostCore は静的 CRT（/MT）で RestTest（/MD）とそろわないので、ソースを直接入れる
target_sources(RestTest PRIVATE Tools/RestTest/RestTest.cpp Source/App/PluginChain.cpp Source/App/PluginGuard.cpp Source/App/ProcessingBuffer.cpp Source/App/PluginScanner.cpp Source/App/GuiResources.cpp Source/App/PluginSafety.cpp
    Source/App/AppLog.cpp Source/HostCore/PluginBinaryCheck.cpp)
target_include_directories(RestTest PRIVATE Source)
target_compile_definitions(RestTest PRIVATE JUCE_PLUGINHOST_VST3=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(RestTest
    PRIVATE
        juce::juce_audio_processors
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

#==============================================================================
# 確認用ツール: 再生デバイスに信号を流し、録音デバイスに届いた音のレベルや遅延を測る
add_executable(ApoProbe Tools/ApoProbe/ApoProbe.cpp)
pri_configure_target(ApoProbe)
target_link_libraries(ApoProbe PRIVATE ole32)

# テスト用ホスト: 本体アプリの代わりに APO とつながり、ゲインをかけて返す
add_executable(HostSim Tools/HostSim/HostSim.cpp)
pri_configure_target(HostSim)
target_link_libraries(HostSim PRIVATE PRISystemRackHostCore)

# 確認用: アプリの音をミュートしても Process Loopback で取り込めるかを調べる
add_executable(LoopbackMuteTest Tools/LoopbackMuteTest/LoopbackMuteTest.cpp)
pri_configure_target(LoopbackMuteTest)
target_link_libraries(LoopbackMuteTest PRIVATE ole32 mmdevapi)
add_library(sanad_android_glue STATIC
    "${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c")
target_include_directories(sanad_android_glue PUBLIC
    "${ANDROID_NDK}/sources/android/native_app_glue")

add_library(sanad_android SHARED AndroidMain.cpp)
target_link_libraries(sanad_android PRIVATE NFRuntime NFAssets NFRendering sanad_android_glue android log dl)
target_link_options(sanad_android PRIVATE "-uANativeActivity_onCreate")
nf_target_set_warnings(sanad_android)

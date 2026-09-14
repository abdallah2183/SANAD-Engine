@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set "VULKAN_SDK=C:\VulkanSDK\1.4.357.0"
set "CMAKE_EXE=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA_EXE=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
cd /d "C:\Users\abdal\OneDrive\Desktop\NOVAForge Engine"
"%CMAKE_EXE%" -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA_EXE%" -DCMAKE_BUILD_TYPE=Debug -B build/debug -DNF_USE_VULKAN=ON -DNF_BUILD_TESTS=ON -DNF_BUILD_SAMPLES=ON %*

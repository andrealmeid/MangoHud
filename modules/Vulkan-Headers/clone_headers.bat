set GIT=%2%
set BUILD_DIR=%3%

set BUILD_DIR=%BUILD_DIR:"=%

if exist "%BUILD_DIR%/registry/vk.xml" goto quit

%GIT% clone --depth 1 https://github.com/KhronosGroup/Vulkan-Headers.git "%BUILD_DIR%"

chdir "%BUILD_DIR%"
xcopy /Y registry\vk.xml .
xcopy /Y /E include\vulkan vulkan\

:quit

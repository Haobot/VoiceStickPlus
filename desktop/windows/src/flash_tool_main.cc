#include "flash_tool_dialog.h"

#include <Windows.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR cmd_line, int show_cmd) {
    voicestick::FlashToolDialog dialog(instance, show_cmd);
    return dialog.Run();
}

#include <flutter/dart_project.h>
#include <flutter/flutter_view_controller.h>
#include <windows.h>

#include "flutter_window.h"
#include "utils.h"

int APIENTRY wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE prev,
                      _In_ wchar_t *command_line, _In_ int show_command) {
  // Attach to console when present (e.g., 'flutter run') or create a
  // new console when running with a debugger.
  if (!::AttachConsole(ATTACH_PARENT_PROCESS) && ::IsDebuggerPresent()) {
    CreateAndAttachConsole();
  }

  // Initialize COM, so that it is available for use in the library and/or
  // plugins.
  ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  flutter::DartProject project(L"data");

  std::vector<std::string> command_line_arguments =
      GetCommandLineArguments();

  project.set_dart_entrypoint_arguments(std::move(command_line_arguments));

  bool created = false;
  {
    // [PATCH A-1] 引擎必须在 COM 仍然有效时析构。
    // 原代码中 window 是函数级局部变量，其析构发生在 return 处，
    // 也就是在 CoUninitialize() 之后。这里加独立作用域，
    // 让 ~FlutterWindow()（销毁引擎、DirectComposition 视觉树）
    // 在 CoUninitialize() 之前完成。
    FlutterWindow window(project);
    Win32Window::Point origin(10, 10);
    Win32Window::Size size(1280, 720);
    if (window.Create(L"simple_live_app", origin, size)) {
      created = true;
      window.SetQuitOnClose(true);

      ::MSG msg;
      while (::GetMessage(&msg, nullptr, 0, 0)) {
        ::TranslateMessage(&msg);
        ::DispatchMessage(&msg);
      }
    }
  }

  if (!created) {
    ::CoUninitialize();
    return EXIT_FAILURE;
  }

  // [PATCH A-2] 关键改动：不再调用 CoUninitialize()。
  // 转储显示崩溃链为 exit() -> RtlExitUserProcess -> LdrShutdownProcess
  // -> 插件的 DLL 静态析构 -> dcomp/coreMessaging -> RaiseFailFastException。
  // 也就是说进程退出时，插件的静态对象仍会 Release COM 对象，
  // 若在此之前已反初始化 COM，就会触发 0xc0000602 弹窗。
  // 进程马上结束，COM 未反初始化不会造成任何影响，交给系统回收即可。
  //
  // 注意：不要改成 exit(0)，那同样会走 RtlExitUserProcess -> LdrShutdownProcess，
  // 会原样复现崩溃（转储已证实）。
  return EXIT_SUCCESS;
}

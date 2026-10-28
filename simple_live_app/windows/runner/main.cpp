#include <cstdlib>

#include <flutter/dart_project.h>
#include <flutter/flutter_view_controller.h>
#include <windows.h>

#include "flutter_window.h"
#include "utils.h"

// [PATCH C] 收尾阶段直接终止进程。
//
// 为什么需要这一步：
//   转储显示 0xc0000602 (RaiseFailFastException) 的真实起因是
//   coremessaging.dll 内部的一次 0xc0000005 访问违例，发生在
//   exit() -> RtlExitUserProcess -> LdrShutdownProcess 阶段，
//   也就是各 DLL 的 DllMain(DETACH) 与 CRT 静态析构过程中。
//   这一阶段在 App 源码里既改不到也拦不住（去掉 CoUninitialize 无效，
//   因为崩溃与 COM 反初始化顺序无关）。
//
//   唯一的确定性做法是在进入该阶段前结束进程：
//   TerminateProcess 不执行 DLL 卸载、不跑 atexit/静态析构，
//   因此不会再有 fail-fast，也就不会弹"已停止工作"。
//
// 数据安全：Hive 在每次写入时即落盘，这里不会丢已保存的配置与关注列表；
//   代价只是最后几毫秒内尚未刷盘的写入。
static void ShutdownImmediately() {
  ::TerminateProcess(::GetCurrentProcess(), 0);
}

int APIENTRY wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE prev,
                      _In_ wchar_t *command_line, _In_ int show_command) {
  // [PATCH C-1] 注册到 CRT 退出表。这样即使将来 Dart/插件某处调用了
  // exit()，也会在进入 LdrShutdownProcess 之前被这里截住。
  std::atexit(ShutdownImmediately);

  // [PATCH C-2] 万一在窗口/引擎析构过程中仍出问题，也不弹系统错误框。
  // 如果以后想恢复崩溃提示以便排查，删掉这一行即可。
  ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
                 SEM_NOOPENFILEERRORBOX);

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
    // 引擎放在独立作用域里，先完整析构，再做后续收尾。
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

  // [PATCH C-3] 正常关闭路径：窗口已销毁，直接结束进程，
  // 不走 exit()/LdrShutdownProcess。
  ShutdownImmediately();

  return EXIT_SUCCESS;
}

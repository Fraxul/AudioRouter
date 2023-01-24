// ApplicationLoopback.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <Windows.h>
#include <sstream>
#include "LoopbackCapture.h"
#include <vector>
#include <map>
#include <psapi.h>


std::map<DWORD, std::wstring> pid_to_image;

void UpdatePIDMap() {
  static std::vector<DWORD> pids(1024, 0);
  pids.resize(pids.capacity());

didResize:
  DWORD pidsSizeNeeded = 0;
  EnumProcesses(pids.data(), (DWORD) (pids.size() * sizeof(DWORD)), &pidsSizeNeeded);
  size_t targetSize = pidsSizeNeeded / sizeof(DWORD);
  if (targetSize == pids.size()) {
    pids.resize(pids.size() * 2);
    goto didResize;
  }
  pids.resize(targetSize);


  // populate a new map so we can drop out PIDs that have disappeared
  std::map<DWORD, std::wstring> pid_to_image_new;
  for (DWORD pid : pids) {
    if (pid == 0)
      continue; // System Idle Process

    {
      auto it = pid_to_image.find(pid);
      if (it != pid_to_image.end()) {
        pid_to_image_new.insert(std::make_pair(it->first, it->second));
        continue; // already tracking this one
      }
    }

    wil::unique_handle hprocess(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, /*inheritHandle=*/ false, pid));
    if (!hprocess)
      continue;

    WCHAR imageFilename[1024];
    GetProcessImageFileName(hprocess.get(), imageFilename, 1023);
    imageFilename[1023] = 0;
    pid_to_image_new.insert(std::make_pair(pid, std::wstring(imageFilename)));
  }
  pid_to_image_new.swap(pid_to_image);
}

DWORD FindPID(const wchar_t* wImageName) {
  for (const auto& it : pid_to_image) {
    // search from the last trailing slash to find the actual filename (we get fully-qualified path)
    size_t off = it.second.rfind(L'\\');
    if (off == std::string::npos)
      off = 0; // start from the beginning
    else
      off += 1; // start after the slash

    if (!lstrcmpiW(wImageName, it.second.data() + off))
      return it.first;
  }
  return 0;
}

BOOL WINAPI DllMain(
  HINSTANCE hinstDLL,  // handle to DLL module
  DWORD fdwReason,     // reason for calling function
  LPVOID lpReserved)  // reserved
{
  // Perform actions based on the reason for calling.
  switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
      // Initialize once for each new process.
      // Return FALSE to fail DLL load.
      break;

    case DLL_THREAD_ATTACH:
      // Do thread-specific initialization.
      break;

    case DLL_THREAD_DETACH:
      // Do thread-specific cleanup.
      break;

    case DLL_PROCESS_DETACH:
      // Perform any necessary cleanup.
      break;
  }
  return TRUE;  // Successful DLL_PROCESS_ATTACH.
}
extern "C" __declspec(dllexport) DWORD __stdcall RouterThread(LPWSTR sourceSpecifier) {
  try {
    THROW_IF_FAILED(Windows::Foundation::Initialize(RO_INIT_MULTITHREADED));
    //OutputDebugStringA("RouterThread");
    //OutputDebugStringW(sourceSpecifier);

    DWORD pid = 0;
    bool attachByName = false;
    {
      wchar_t* endptr = nullptr;
      pid = wcstoul(sourceSpecifier, &endptr, 10);
      if (*endptr != 0) { // conversion failed
        pid = 0;
        attachByName = true;
      }
    }

    CLoopbackCapture loopbackCapture;


    if (attachByName) {
      while (true) {
        wil::unique_handle hProcess;

        while (!pid && !hProcess) {
          UpdatePIDMap();
          pid = FindPID(sourceSpecifier);
          if (pid == 0) {
            Sleep(1000);
            continue;
          }

          hProcess.reset(OpenProcess(SYNCHRONIZE, false, pid));
          if (!hProcess) {
            std::wstringstream ss;
            ss << L"AudioRouter: OpenProcess() failed for PID " << pid;
            OutputDebugStringW(ss.str().c_str());
            pid = 0;
            continue;
          }
        }

        {
          std::wstringstream ss;
          ss << L"AudioRouter: Attached to PID " << pid;
          OutputDebugStringW(ss.str().c_str());
        }
        loopbackCapture.StartCaptureAsync(pid);

        WaitForSingleObject(hProcess.get(), INFINITE);
        OutputDebugStringW(L"AudioRouter: Attached process terminated.");

        loopbackCapture.StopCaptureAsync();
        hProcess.reset();
        pid = 0;
      }

    } else {
      // One-shot, by PID
      wil::unique_handle hProcess(OpenProcess(SYNCHRONIZE, false, pid));
      THROW_LAST_ERROR_IF_NULL(hProcess);
      loopbackCapture.StartCaptureAsync(pid);
      WaitForSingleObject(hProcess.get(), INFINITE);
      OutputDebugStringW(L"AudioRouter: Attached process terminated.");
      loopbackCapture.StopCaptureAsync();
    }

  } catch (const std::exception& ex) {
    OutputDebugStringA(ex.what());
  }
  return 0;
}

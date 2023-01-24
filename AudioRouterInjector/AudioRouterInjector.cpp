#include <iostream>
#include <Windows.h>
#include <Psapi.h>
#include <map>
#include <string>
#include <vector>

#include <wrl\implements.h>
#include <wil\com.h>
#include <wil\result.h>

std::map<DWORD, std::wstring> pid_to_image;

void UpdatePIDMap() {
  static std::vector<DWORD> pids(1024, 0);
  pids.resize(pids.capacity());

didResize:
  DWORD pidsSizeNeeded = 0;
  EnumProcesses(pids.data(), (DWORD)(pids.size() * sizeof(DWORD)), &pidsSizeNeeded);
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

int wmain(int argc, wchar_t* argv[]) {

  if (argc <= 2) {
    printf("Usage: AudioRouterInjector target-imagename-or-pid source-imagename-or-pid\n");
    printf("Routes audio from source to target.\n");
    printf("If source is an imagename, routing will automatically be (re)attached when the process starts.\n");
    printf("Image names are EXE filenames, like \"notepad.exe\"\n");
    return -1;
  }


  wchar_t* targetSpecifier = argv[1];
  wchar_t* sourceSpecifier = argv[2];

  DWORD pid = 0;
  {
    wchar_t* endptr = nullptr;
    pid = wcstoul(targetSpecifier, &endptr, 10);
    if (*endptr != 0) // conversion failed
      pid = 0;
  }

  if (pid <= 0) {
    UpdatePIDMap();
    pid = FindPID(targetSpecifier);
    if (pid <= 0) {
      printf("Couldn't find a running process matching \"%S\"\n", targetSpecifier);
      return -1;
    }
  }

  HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid); // TODO verify process access requirements
  if (!hProcess) {
    printf("Couldn't open PID %d\n", pid);
    RETURN_LAST_ERROR_IF_NULL(hProcess);
  }

  HMODULE hDll = LoadLibraryW(L"AudioRouter.dll");
  if (!hDll) {
    printf("Couldn't open DLL %d\n", pid);
    RETURN_LAST_ERROR_IF_NULL(hDll);
  }

  uintptr_t routerThreadEntryPoint = (uintptr_t)GetProcAddress(hDll, "RouterThread");
  if (routerThreadEntryPoint == 0) {
    printf("Couldn't find entry point in DLL\n");
    return -1;
  }

  const size_t dllFilenameLen = 32768;
  TCHAR* dllFilename = new TCHAR[32768];
  memset(dllFilename, 0, sizeof(TCHAR) * dllFilenameLen);

  RETURN_LAST_ERROR_IF(0 == GetModuleFileNameW(hDll, dllFilename, 32768));
  printf("DLL filename: %S\n", dllFilename);

  // Allocate memory for the dllpath in the target process, length of the path string + null terminator
  LPVOID pDllPath = VirtualAllocEx(hProcess, 0, dllFilenameLen * sizeof(TCHAR), MEM_COMMIT, PAGE_READWRITE);
  RETURN_LAST_ERROR_IF_NULL(pDllPath);

  // Write the path to the address of the memory we just allocated in the target process
  RETURN_LAST_ERROR_IF(!WriteProcessMemory(hProcess, pDllPath, (LPVOID)dllFilename, dllFilenameLen * sizeof(TCHAR), 0));

  // Create a Remote Thread in the target process which calls LoadLibraryW on the DLL path we copied over.
  // Note that kernel32's base address (HMODULE) is globally consistent, so we can call GetModuleHandle/GetProcAddress in this process to find the
  // offset of LoadLibraryW for the remote target process.
  HANDLE hLoadThread = CreateRemoteThread(hProcess, 0, 0, (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleA("Kernel32.dll"), "LoadLibraryW"), pDllPath, 0, 0);
  RETURN_LAST_ERROR_IF_NULL(hLoadThread);

  WaitForSingleObject(hLoadThread, INFINITE); // Wait for the execution of our loader thread to finish
  CloseHandle(hLoadThread);
  // DWORD exitCode = 0;
  // GetExitCodeThread(hLoadThread, &exitCode);
  // printf("LoadLibraryW thread exited with code %x\n", exitCode);

  VirtualFreeEx(hProcess, pDllPath, 0, MEM_RELEASE); // Free the memory allocated for our dll path

  // Enumerate remote modules
  std::vector<HMODULE> hModules(128, nullptr);
  while (true) {
    DWORD hModulesSizeBytes = (DWORD) (hModules.size() * sizeof(HMODULE));
    DWORD actualSize = 0;
    EnumProcessModules(hProcess, hModules.data(), (DWORD) (hModules.size() * sizeof(HMODULE)), &actualSize);
    if (hModulesSizeBytes <= actualSize) {
      hModules.resize(hModules.size() * 2);
      continue;
    } else {
      hModules.resize(actualSize / sizeof(HMODULE));
      break;
    }
  }

  HMODULE remoteDllModule = nullptr;
  {
    const size_t moduleNameLen = 32768;
    TCHAR* moduleName = new TCHAR[moduleNameLen];
    

    for (HMODULE hModule : hModules) {
      if (GetModuleFileNameEx(hProcess, hModule, moduleName, moduleNameLen)) {
        // printf("Checking module %S\n", moduleName);
        if (!lstrcmpiW(moduleName, dllFilename)) {
          remoteDllModule = hModule;
          break;
        }
      }
    }

    if (remoteDllModule == nullptr) {
      printf("Unable to locate the loaded DLL in the target process's module list\n");
      return -1;
    }
    delete[] moduleName;
  }

  // Offset from the RouterThread entrypoint to the hModule of the DLL in this process
  ptrdiff_t entryOffset = (routerThreadEntryPoint) - ((uintptr_t) hDll);
  uintptr_t remoteEntry = entryOffset + ((uintptr_t) remoteDllModule);

  printf("Local address: %p\n", (void*) routerThreadEntryPoint);
  printf("Local module base: %p\n", hDll);
  printf("Remote module base: %p\n", remoteDllModule);
  printf("Remote entry: %p\n", (void*) remoteEntry);
  printf("Entry offset: %tx\n", entryOffset);


  // Copy source specifier to the target process
  size_t sourceSpecifierLengthBytes = (lstrlenW(sourceSpecifier) + 1) * sizeof(WCHAR);
  LPVOID pSourceSpecifier = VirtualAllocEx(hProcess, 0, sourceSpecifierLengthBytes, MEM_COMMIT, PAGE_READWRITE);
  RETURN_LAST_ERROR_IF_NULL(pSourceSpecifier);
  RETURN_LAST_ERROR_IF(!WriteProcessMemory(hProcess, pSourceSpecifier, (LPVOID)sourceSpecifier, sourceSpecifierLengthBytes, 0));

  // Run the router thread with the source specifier as the argument
  HANDLE hRouterThread = CreateRemoteThread(hProcess, 0, 0, (LPTHREAD_START_ROUTINE) remoteEntry, pSourceSpecifier, 0, 0);
  RETURN_LAST_ERROR_IF_NULL(hRouterThread);
  printf("AudioRouter thread is running...\n");
  WaitForSingleObject(hRouterThread, INFINITE); // Wait for the router thread to finish.

  printf("AudioRouter thread has exited.\n");
  CloseHandle(hRouterThread);
  VirtualFreeEx(hProcess, pSourceSpecifier, 0, MEM_RELEASE);

  return 0;
}
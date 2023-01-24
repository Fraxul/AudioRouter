# AudioRouter
Clone audio between processes using Application Loopback audio capture.

This commandline tool injects a DLL into one process ("target process") that copies audio from another process ("source process")
and plays it through the first non-default audio output device. The intended use-case is to make audio available for
Discord window streaming.

Limitations:
  - According to the MSDN documentation for `AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS`, you need to be running Windows 10 build 20348 or later, or Windows 11.
    - Tested/working on Windows 10 19044.2486 with Windows Feature Experience Pack 120.2212.4190.0
  - 64-bit only. It'd probably work on 32-bit processes if you built a 32-bit version, though.
  - You need to have an audio output device that's not hooked up to anything to play the audio to, unless you want to hear it twice.


Usage:
`AudioRouterInjector.exe target-specifier source-specifier`
  - target-specifier and source-specifier are either an image name ("notepad.exe") or a PID ("1234")
  - target-specifier must be running; the injector will not wait for a process to start.
  - If source-specifier is a PID, it must be running. The router will attach once and self-terminate once the source process exits.
  - If source-specifier is an image name, the router DLL will wait for it to start, attach to it, and attempt to reattach when it is terminated.


Sample invocations:
  - `.\AudioRouterInjector.exe capture.exe Ragnarock-Win64-Shipping.exe`
    - Copy the audio from the game Ragnarock to `capture.exe` which is the LIV compositor. (Their "Discord Audio" router doesn't work correctly on my machine.)
  - `.\AudioRouterInjector.exe notepad.exe vlc.exe`
    - Copy the audio from VLC Media Player to Notepad. I'm not sure why you'd want to do this, but now it's possible!


The injector tool logs to the console. The DLL uses `OutputDebugString`; logs from it can be viewed with [DebugViewPP](https://github.com/CobaltFusion/DebugViewPP)

Largely based on [this Microsoft sample code](https://learn.microsoft.com/en-us/samples/microsoft/windows-classic-samples/applicationloopbackaudio-sample/).

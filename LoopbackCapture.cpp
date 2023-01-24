#include <shlobj.h>
#include <wchar.h>
#include <audioclientactivationparams.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <cassert>

#include "LoopbackCapture.h"

#define BITS_PER_BYTE 8

HRESULT CLoopbackCapture::SetDeviceStateErrorIfFailed(HRESULT hr) {
  if (FAILED(hr)) {
    m_DeviceState = DeviceState::Error;
  }
  return hr;
}

CLoopbackCapture::CLoopbackCapture() {
  // Create events for sample ready or user stop
  THROW_IF_FAILED(m_SampleReadyEvent.create(wil::EventOptions::None));

  // Initialize MF
  THROW_IF_FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE));

  // Register MMCSS work queue
  DWORD dwTaskID = 0;
  THROW_IF_FAILED(MFLockSharedWorkQueue(L"Capture", 0, &dwTaskID, &m_dwQueueID));

  // Set the capture event work queue to use the MMCSS queue
  m_xSampleReady.SetQueueID(m_dwQueueID);

  // Create the completion event as auto-reset
  THROW_IF_FAILED(m_hActivateCompleted.create(wil::EventOptions::None));

  // Create the capture-stopped event as auto-reset
  THROW_IF_FAILED(m_hCaptureStopped.create(wil::EventOptions::None));

  SelectAudioDevice();
}

void CLoopbackCapture::SelectAudioDevice() {
  wil::com_ptr<IMMDeviceEnumerator> enumerator = wil::CoCreateInstance<MMDeviceEnumerator, IMMDeviceEnumerator>(CLSCTX_ALL);


  wil::com_ptr<IMMDevice> defaultAudioEndpoint;
  THROW_IF_FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, defaultAudioEndpoint.put()));
  wil::unique_cotaskmem_string defaultAudioEndpointIdStr;
  THROW_IF_FAILED(defaultAudioEndpoint->GetId(defaultAudioEndpointIdStr.put()));


  wil::com_ptr<IMMDeviceCollection> deviceCollection;
  THROW_IF_FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, deviceCollection.put()));
  UINT deviceCount;

  m_audioOutputDevice.reset(); // clear any previous device selection

  THROW_IF_FAILED(deviceCollection->GetCount(&deviceCount));
  for (UINT deviceIdx = 0; deviceIdx < deviceCount; ++deviceIdx) {
    wil::com_ptr<IMMDevice> device;
    THROW_IF_FAILED(deviceCollection->Item(deviceIdx, device.put()));

    wil::unique_cotaskmem_string deviceIdStr;
    THROW_IF_FAILED(device->GetId(deviceIdStr.put()));

    wil::com_ptr<IPropertyStore> devicePropertyStore;
    THROW_IF_FAILED(device->OpenPropertyStore(STGM_READ, devicePropertyStore.put()));

    wil::unique_prop_variant deviceFriendlyName;
    THROW_IF_FAILED(devicePropertyStore->GetValue(PKEY_Device_FriendlyName, deviceFriendlyName.addressof()));


    char buf[512];
    snprintf(buf, 512, "AudioRouter: Endpoint %u: \"%S\" (%S)", deviceIdx, deviceFriendlyName.pwszVal, deviceIdStr.get());
    OutputDebugStringA(buf);

    if (m_audioOutputDevice == nullptr) {
      if (lstrcmpW(deviceIdStr.get(), defaultAudioEndpointIdStr.get()) != 0) {
        OutputDebugStringA("  - Using this endpoint, since it's the first non-default render device available.");
        m_audioOutputDevice = device;
      }
    }
  }

  if (!m_audioOutputDevice) {
    OutputDebugStringA("Only one audio output device and it's the default one.");
    m_audioOutputDevice = defaultAudioEndpoint;
  }

  THROW_IF_FAILED(m_audioOutputDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, m_audioClientForOutput.put_void()));
  THROW_IF_FAILED(m_audioClientForOutput->GetMixFormat(m_waveFormat.put()));
  THROW_IF_FAILED(m_audioClientForOutput->Initialize(AUDCLNT_SHAREMODE_SHARED, /*streamFlags=*/ 0,
    /*bufferDuration (ns)=*/ 1000000,
    /*periodicity (ns)=*/ 0,
    m_waveFormat.get(),
    /*audioSessionGuid=*/ nullptr));
  THROW_IF_FAILED(m_audioClientForOutput->GetBufferSize(&m_renderBufferSizeFrames));
  THROW_IF_FAILED(m_audioClientForOutput->GetService(__uuidof(IAudioRenderClient), m_audioRenderClient.put_void()));
}

CLoopbackCapture::~CLoopbackCapture() {
  if (m_dwQueueID != 0) {
    MFUnlockWorkQueue(m_dwQueueID);
  }
}

void CLoopbackCapture::ActivateAudioInterface(DWORD processId) {
  AUDIOCLIENT_ACTIVATION_PARAMS audioclientActivationParams = {};
  audioclientActivationParams.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
  audioclientActivationParams.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
  audioclientActivationParams.ProcessLoopbackParams.TargetProcessId = processId;

  PROPVARIANT activateParams = {};
  activateParams.vt = VT_BLOB;
  activateParams.blob.cbSize = sizeof(audioclientActivationParams);
  activateParams.blob.pBlobData = (BYTE*)&audioclientActivationParams;

  wil::com_ptr<IActivateAudioInterfaceAsyncOperation> asyncOp;
  THROW_IF_FAILED(ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &activateParams, this, &asyncOp));

  // Wait for activation completion
  m_hActivateCompleted.wait();

  THROW_IF_FAILED(m_activateResult);
}

//
//  ActivateCompleted()
//
//  Callback implementation of ActivateAudioInterfaceAsync function.  This will be called on MTA thread
//  when results of the activation are available.
//
HRESULT CLoopbackCapture::ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) {
  m_activateResult = SetDeviceStateErrorIfFailed([&]()->HRESULT {
    // Check for a successful activation result
    HRESULT hrActivateResult = E_UNEXPECTED;
  wil::com_ptr_nothrow<IUnknown> punkAudioInterface;
  RETURN_IF_FAILED(operation->GetActivateResult(&hrActivateResult, &punkAudioInterface));
  RETURN_IF_FAILED(hrActivateResult);

  // Get the pointer for the Audio Client
  RETURN_IF_FAILED(punkAudioInterface.copy_to(&m_AudioClient));

  // The app can also call m_AudioClient->GetMixFormat instead to get the capture format.
  // 16 - bit PCM format.
#if 0
  m_CaptureFormat.wFormatTag = WAVE_FORMAT_PCM;
  m_CaptureFormat.nChannels = 2;
  m_CaptureFormat.nSamplesPerSec = 44100;
  m_CaptureFormat.wBitsPerSample = 16;
  m_CaptureFormat.nBlockAlign = m_CaptureFormat.nChannels * m_CaptureFormat.wBitsPerSample / BITS_PER_BYTE;
  m_CaptureFormat.nAvgBytesPerSec = m_CaptureFormat.nSamplesPerSec * m_CaptureFormat.nBlockAlign;
#endif

  // Initialize the AudioClient in Shared Mode with the user specified buffer
  RETURN_IF_FAILED(m_AudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                             AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                             200000,
                                             AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                                             m_waveFormat.get(),
                                             nullptr));

  // Get the maximum size of the AudioClient Buffer
  RETURN_IF_FAILED(m_AudioClient->GetBufferSize(&m_BufferFrames));

  // Get the capture client
  RETURN_IF_FAILED(m_AudioClient->GetService(IID_PPV_ARGS(&m_AudioCaptureClient)));

  // Create Async callback for sample events
  RETURN_IF_FAILED(MFCreateAsyncResult(nullptr, &m_xSampleReady, nullptr, &m_SampleReadyAsyncResult));

  // Tell the system which event handle it should signal when an audio buffer is ready to be processed by the client
  RETURN_IF_FAILED(m_AudioClient->SetEventHandle(m_SampleReadyEvent.get()));

  // Everything is ready.
  m_DeviceState = DeviceState::Initialized;

  return S_OK;
                                                 }());

  // Let ActivateAudioInterface know that m_activateResult has the result of the activation attempt.
  m_hActivateCompleted.SetEvent();
  return S_OK;
}


void CLoopbackCapture::StartCaptureAsync(DWORD processId) {
  ActivateAudioInterface(processId);

  // We should be in the initialzied state if this is the first time through getting ready to capture.
  if (m_DeviceState == DeviceState::Initialized) {
    m_DeviceState = DeviceState::Starting;
    THROW_IF_FAILED(MFPutWorkItem2(MFASYNC_CALLBACK_QUEUE_MULTITHREADED, 0, &m_xStartCapture, nullptr));
  }
}

//
//  OnStartCapture()
//
//  Callback method to start capture
//
HRESULT CLoopbackCapture::OnStartCapture(IMFAsyncResult* pResult) {
  return SetDeviceStateErrorIfFailed([&]()->HRESULT {
    // Start the capture
    RETURN_IF_FAILED(m_AudioClient->Start());

    RETURN_IF_FAILED(m_audioClientForOutput->Start());

    m_DeviceState = DeviceState::Capturing;
    MFPutWaitingWorkItem(m_SampleReadyEvent.get(), 0, m_SampleReadyAsyncResult.get(), &m_SampleReadyKey);

    return S_OK;
   }());
}


//
//  StopCaptureAsync()
//
//  Stop capture asynchronously via MF Work Item
//
void CLoopbackCapture::StopCaptureAsync() {
  THROW_HR_IF(E_NOT_VALID_STATE, (m_DeviceState != DeviceState::Capturing) &&
               (m_DeviceState != DeviceState::Error));

  m_DeviceState = DeviceState::Stopping;

  THROW_IF_FAILED(MFPutWorkItem2(MFASYNC_CALLBACK_QUEUE_MULTITHREADED, 0, &m_xStopCapture, nullptr));

  // Wait for capture to stop
  m_hCaptureStopped.wait();

  m_audioClientForOutput->Stop();
}

//
//  OnStopCapture()
//
//  Callback method to stop capture
//
HRESULT CLoopbackCapture::OnStopCapture(IMFAsyncResult* pResult) {
  // Stop capture by cancelling Work Item
  // Cancel the queued work item (if any)
  if (0 != m_SampleReadyKey) {
    MFCancelWorkItem(m_SampleReadyKey);
    m_SampleReadyKey = 0;
  }

  m_AudioClient->Stop();
  m_SampleReadyAsyncResult.reset();

  return FinishCaptureAsync();
}

//
//  FinishCaptureAsync()
//
//  Finalizes WAV file on a separate thread via MF Work Item
//
HRESULT CLoopbackCapture::FinishCaptureAsync() {
  // We should be flushing when this is called
  return MFPutWorkItem2(MFASYNC_CALLBACK_QUEUE_MULTITHREADED, 0, &m_xFinishCapture, nullptr);
}

//
//  OnFinishCapture()
//
//  Because of the asynchronous nature of the MF Work Queues and the DataWriter, there could still be
//  a sample processing.  So this will get called to finalize the WAV header.
//
HRESULT CLoopbackCapture::OnFinishCapture(IMFAsyncResult* pResult) {

  // TODO stop and blank output

  m_DeviceState = DeviceState::Stopped;

  m_hCaptureStopped.SetEvent();

  return S_OK;
}

//
//  OnSampleReady()
//
//  Callback method when ready to fill sample buffer
//
HRESULT CLoopbackCapture::OnSampleReady(IMFAsyncResult* pResult) {
  if (SUCCEEDED(OnAudioSampleRequested())) {
    // Re-queue work item for next sample
    if (m_DeviceState == DeviceState::Capturing) {
      // Re-queue work item for next sample
      return MFPutWaitingWorkItem(m_SampleReadyEvent.get(), 0, m_SampleReadyAsyncResult.get(), &m_SampleReadyKey);
    }
  } else {
    m_DeviceState = DeviceState::Error;
  }

  return S_OK;
}

//
//  OnAudioSampleRequested()
//
//  Called when audio device fires m_SampleReadyEvent
//
HRESULT CLoopbackCapture::OnAudioSampleRequested() {
  UINT32 FramesAvailable = 0;
  BYTE* Data = nullptr;
  DWORD dwCaptureFlags;
  UINT64 u64DevicePosition = 0;
  UINT64 u64QPCPosition = 0;
  DWORD cbBytesToCapture = 0;

  auto lock = m_CritSec.lock();

  // If this flag is set, we have already queued up the async call to end the capture.
  if (m_DeviceState == DeviceState::Stopping) {
    return S_OK;
  }

  // A word on why we have a loop here;
  // Suppose it has been 10 milliseconds or so since the last time
  // this routine was invoked, and that we're capturing 48000 samples per second.
  //
  // The audio engine can be reasonably expected to have accumulated about that much
  // audio data - that is, about 480 samples.
  //
  // However, the audio engine is free to accumulate this in various ways:
  // a. as a single packet of 480 samples, OR
  // b. as a packet of 80 samples plus a packet of 400 samples, OR
  // c. as 48 packets of 10 samples each.
  //
  // In particular, there is no guarantee that this routine will be
  // run once for each packet.
  //
  // So every time this routine runs, we need to read ALL the packets
  // that are now available;
  //
  // We do this by calling IAudioCaptureClient::GetNextPacketSize
  // over and over again until it indicates there are no more packets remaining.
  while (SUCCEEDED(m_AudioCaptureClient->GetNextPacketSize(&FramesAvailable)) && FramesAvailable > 0) {
    cbBytesToCapture = FramesAvailable * m_waveFormat.get()->nBlockAlign;
    assert(cbBytesToCapture > 0);

    // Get sample buffer
    RETURN_IF_FAILED(m_AudioCaptureClient->GetBuffer(&Data, &FramesAvailable, &dwCaptureFlags, &u64DevicePosition, &u64QPCPosition));


    // Copy data to the render buffer
    BYTE* outputBuffer = nullptr;
    RETURN_IF_FAILED(m_audioRenderClient->GetBuffer(FramesAvailable, &outputBuffer));
    memcpy(outputBuffer, Data, cbBytesToCapture);
    m_audioRenderClient->ReleaseBuffer(FramesAvailable, 0);


    // Write File
#if 0
    if (m_DeviceState != DeviceState::Stopping) {
      DWORD dwBytesWritten = 0;
      RETURN_IF_WIN32_BOOL_FALSE(WriteFile(
        m_hFile.get(),
        Data,
        cbBytesToCapture,
        &dwBytesWritten,
        NULL));
    }
#endif

    // Release buffer back
    m_AudioCaptureClient->ReleaseBuffer(FramesAvailable);
  }

  return S_OK;
}

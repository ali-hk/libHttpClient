// Copyright (c) Microsoft Corporation
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#pragma once

#include "DeviceResources.h"
#include "StepTimer.h"
#include "SampleGUI.h"
#include "httpClient/httpClient.h"

// A basic sample implementation that creates a D3D11 device and
// provides a render loop.
class Sample
{
public:

    Sample();

    // Initialization and management
    void Initialize(IUnknown* window);

    // Basic game loop
    void Tick();

    // Messages
    void OnSuspending();
    void OnResuming();


private:

    void Update(DX::StepTimer const& timer);
    void Render();

    void Clear();

    void CreateDeviceDependentResources();
    void CreateWindowSizeDependentResources();

    void SetupUI();

    void StartBackgroundThread();
    void StopBackgroundThread();
    void MakeHttpCall();
    void MakeWebsocket();
    void SendMessage();

    static void BinaryReceiveHandler(
        _In_ HCWebsocketHandle websocket,
        _In_reads_bytes_(payloadSize) const uint8_t* payloadBytes,
        _In_ uint32_t payloadSize,
        _In_ void* functionContext
    ); 
    
    static void StringReceiveHandler(
        _In_ HCWebsocketHandle websocket,
        _In_z_ const char* incomingBodyString,
        _In_ void* functionContext
    );
    HANDLE m_hBackgroundThread;

    // Device resources.
    std::unique_ptr<DX::DeviceResources>        m_deviceResources;

    // Rendering loop timer.
    uint64_t                                    m_frame;
    DX::StepTimer                               m_timer;

    // Input devices.
    std::unique_ptr<DirectX::GamePad>           m_gamePad;

    DirectX::GamePad::ButtonStateTracker        m_gamePadButtons;

    // DirectXTK objects.
    std::unique_ptr<DirectX::GraphicsMemory>    m_graphicsMemory;

    // UI Objects
    std::shared_ptr<ATG::UIManager>             m_ui;
    std::unique_ptr<DX::TextConsole>            m_console;

    XTaskQueueHandle m_queue;
    XTaskQueueRegistrationToken m_callbackToken;
};

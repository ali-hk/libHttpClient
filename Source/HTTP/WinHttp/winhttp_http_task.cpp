// Copyright (c) Microsoft Corporation
// Licensed under the MIT license. See LICENSE file in the project root for full license information.
#include "pch.h"
#include <winhttp.h>
#include "../httpcall.h"
#include "uri.h"
#include "winhttp_http_task.h"
#include <schannel.h>

#if !HC_NOWEBSOCKETS
#include "hcwebsocket.h"
#endif

#if HC_PLATFORM == HC_PLATFORM_GSDK
#include "XGameRuntimeFeature.h"
#include <winsock2.h>
#include <iphlpapi.h>
#endif

#define CRLF L"\r\n"

using namespace xbox::httpclient;

#define WINHTTP_WEBSOCKET_RECVBUFFER_SIZE (1024 * 4)
#define WINHTTP_WEBSOCKET_RECVBUFFER_MAXSIZE (1024 * 20)

void get_proxy_name(
    _In_ xbox::httpclient::proxy_type proxyType,
    _In_ xbox::httpclient::Uri proxyUri,
    _Out_ DWORD* pAccessType,
    _Out_ http_internal_wstring* pwProxyName);

HC_PERFORM_ENV::HC_PERFORM_ENV()
{
    xbox::httpclient::Uri proxyUri;
    m_proxyType = get_ie_proxy_info(proxy_protocol::https, proxyUri);

    DWORD accessType = WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;
    http_internal_wstring wProxyName;
    get_proxy_name(m_proxyType, proxyUri, &accessType, &wProxyName);

    m_hSession = WinHttpOpen(
        nullptr,
        accessType,
        wProxyName.length() > 0 ? wProxyName.c_str() : WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        WINHTTP_FLAG_ASYNC);
}

HC_PERFORM_ENV::~HC_PERFORM_ENV()
{
    if (m_hSession != nullptr)
    {
        WinHttpCloseHandle(m_hSession);
    }
}

void get_proxy_name(
    _In_ proxy_type proxyType,
    _In_ xbox::httpclient::Uri proxyUri,
    _Out_ DWORD* pAccessType,
    _Out_ http_internal_wstring* pwProxyName)
{
    switch (proxyType)
    {
        case proxy_type::no_proxy:
        {
            *pAccessType = WINHTTP_ACCESS_TYPE_NO_PROXY;
            *pwProxyName = L"";
            break;
        }

        case proxy_type::named_proxy:
        {
            *pAccessType = WINHTTP_ACCESS_TYPE_NAMED_PROXY;

            http_internal_wstring wProxyHost = utf16_from_utf8(proxyUri.Host());

            // WinHttpOpen cannot handle trailing slash in the name, so here is some string gymnastics to keep WinHttpOpen happy
            if (proxyUri.IsPortDefault())
            {
                *pwProxyName = wProxyHost;
            }
            else
            {
                if (proxyUri.Port() > 0)
                {
                    http_internal_basic_stringstream<wchar_t> ss;
                    ss.imbue(std::locale::classic());
                    ss << wProxyHost << L":" << proxyUri.Port();
                    *pwProxyName = ss.str().c_str();
                }
                else
                {
                    *pwProxyName = wProxyHost;
                }
            }
            break;
        }

        default:
        case proxy_type::autodiscover_proxy:
        case proxy_type::default_proxy:
        {
            *pAccessType = WINHTTP_ACCESS_TYPE_DEFAULT_PROXY;
            *pwProxyName = L"";
            break;
        }
    }
}

NAMESPACE_XBOX_HTTP_CLIENT_BEGIN

winhttp_http_task::winhttp_http_task(
    _Inout_ XAsyncBlock* asyncBlock,
    _In_ HCCallHandle call,
    _In_ HINTERNET hSession,
    _In_ proxy_type proxyType,
    _In_ bool isWebSocket
    ) :
    m_call(call),
    m_asyncBlock(asyncBlock),
    m_hSession(hSession),
    m_proxyType(proxyType),
    m_isWebSocket(isWebSocket)
{
#if HC_WINHTTP_WEBSOCKETS
    m_hWebsocketWriteComplete = CreateEvent(nullptr, false, false, nullptr);
#endif
}

winhttp_http_task::~winhttp_http_task()
{
    HC_TRACE_VERBOSE(HTTPCLIENT, "winhttp_http_task dtor");

#if HC_WINHTTP_WEBSOCKETS
    if (m_socketState == WinHttpWebsockState::Connected ||
        m_socketState == WinHttpWebsockState::Connecting)
    {
        disconnect_websocket(HCWebSocketCloseStatus::Normal);
    }
#endif

    if (m_hRequest != nullptr)
    {
        WinHttpCloseHandle(m_hRequest);
    }
    if (m_hConnection != nullptr)
    {
        WinHttpCloseHandle(m_hConnection);
    }
#if HC_WINHTTP_WEBSOCKETS
    if (m_hWebsocketWriteComplete != nullptr)
    {
        CloseHandle(m_hWebsocketWriteComplete);
    }
#endif
}

void winhttp_http_task::complete_task(_In_ HRESULT translatedHR)
{
    complete_task(translatedHR, translatedHR);
}

void winhttp_http_task::complete_task(_In_ HRESULT translatedHR, uint32_t platformSpecificError)
{
    if (m_asyncBlock != nullptr)
    {
#if HC_WINHTTP_WEBSOCKETS
        if (m_isWebSocket)
        {
            m_connectHr = translatedHR;
            m_connectPlatformError = platformSpecificError;
            XAsyncComplete(m_asyncBlock, S_OK, sizeof(WebSocketCompletionResult));
        }
        else
#endif
        {
            HCHttpCallResponseSetNetworkErrorCode(m_call, translatedHR, platformSpecificError);
            XAsyncComplete(m_asyncBlock, S_OK, 0);
        }
        m_asyncBlock = nullptr;
    }

    if (m_hRequest != nullptr && !m_isWebSocket)
    {
        WinHttpSetStatusCallback(m_hRequest, nullptr, WINHTTP_CALLBACK_FLAG_ALL_NOTIFICATIONS, NULL);
        shared_ptr_cache::remove(this);
    }
}

// Helper function to query/read next part of response data from winhttp.
void winhttp_http_task::read_next_response_chunk(_In_ winhttp_http_task* pRequestContext, DWORD /*bytesRead*/)
{
    if (!WinHttpQueryDataAvailable(pRequestContext->m_hRequest, nullptr))
    {
        DWORD dwError = GetLastError();
        HC_TRACE_ERROR(HTTPCLIENT, "HCHttpCallPerform [ID %llu] [TID %ul] WinHttpQueryDataAvailable errorcode %d", HCHttpCallGetId(pRequestContext->m_call), GetCurrentThreadId(), dwError);
        pRequestContext->complete_task(E_FAIL, HRESULT_FROM_WIN32(dwError));
    }
}

void winhttp_http_task::_multiple_segment_write_data(_In_ winhttp_http_task* pRequestContext)
{
    const uint64_t defaultChunkSize = 64 * 1024;
    uint64_t safeSize = std::min(pRequestContext->m_requestBodyRemainingToWrite, defaultChunkSize);

    const BYTE* requestBody = nullptr;
    uint32_t requestBodyBytes = 0;
    if (HCHttpCallRequestGetRequestBodyBytes(pRequestContext->m_call, &requestBody, &requestBodyBytes) != S_OK)
    {
        return;
    }

    if( !WinHttpWriteData(
        pRequestContext->m_hRequest,
        &requestBody[pRequestContext->m_requestBodyOffset],
        static_cast<DWORD>(safeSize),
        nullptr))
    {
        DWORD dwError = GetLastError();
        HC_TRACE_ERROR(HTTPCLIENT, "HCHttpCallPerform [ID %llu] [TID %ul] WinHttpWriteData errorcode %d", HCHttpCallGetId(pRequestContext->m_call), GetCurrentThreadId(), dwError);
        pRequestContext->complete_task(E_FAIL, HRESULT_FROM_WIN32(dwError));
        return;
    }

    // Stop writing chunks after this one if no more data.
    pRequestContext->m_requestBodyRemainingToWrite -= safeSize;
    if (pRequestContext->m_requestBodyRemainingToWrite == 0)
    {
        pRequestContext->m_requestBodyType = msg_body_type::no_body;
    }
    pRequestContext->m_requestBodyOffset += safeSize;
}

void winhttp_http_task::callback_status_write_complete(
    _In_ HINTERNET hRequestHandle,
    _In_ winhttp_http_task* pRequestContext,
    _In_ void* statusInfo)
{
    DWORD bytesWritten = *((DWORD *)statusInfo);
    HC_TRACE_INFORMATION(HTTPCLIENT, "HCHttpCallPerform [ID %llu] [TID %ul] WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE bytesWritten=%d", HCHttpCallGetId(pRequestContext->m_call), GetCurrentThreadId(), bytesWritten);

    if (pRequestContext->m_requestBodyType == content_length_chunked)
    {
        _multiple_segment_write_data(pRequestContext);
    }
    else
    {
        if (!WinHttpReceiveResponse(hRequestHandle, nullptr))
        {
            DWORD dwError = GetLastError();
            HC_TRACE_ERROR(HTTPCLIENT, "HCHttpCallPerform [ID %llu] [TID %ul] WinHttpReceiveResponse errorcode %d", HCHttpCallGetId(pRequestContext->m_call), GetCurrentThreadId(), dwError);
            pRequestContext->complete_task(E_FAIL, HRESULT_FROM_WIN32(dwError));
            return;
        }
    }
}

void winhttp_http_task::callback_status_request_error(
    _In_ HINTERNET hRequestHandle,
    _In_ winhttp_http_task* pRequestContext,
    _In_ void* statusInfo)
{
    WINHTTP_ASYNC_RESULT *error_result = reinterpret_cast<WINHTTP_ASYNC_RESULT *>(statusInfo);
    if (error_result == nullptr)
        return;

    DWORD errorCode = error_result->dwError;
    HC_TRACE_ERROR(HTTPCLIENT, "HCHttpCallPerform [ID %llu] [TID %ul] WINHTTP_CALLBACK_STATUS_REQUEST_ERROR dwResult=%d dwError=%d", HCHttpCallGetId(pRequestContext->m_call), GetCurrentThreadId(), error_result->dwResult, error_result->dwError);

    bool reissueSend{ false };

    if (error_result->dwError == ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED)
    {
        SecPkgContext_IssuerListInfoEx* pIssuerList{ nullptr };
        DWORD dwBufferSize{ sizeof(void*) };

        if (WinHttpQueryOption(
            hRequestHandle,
            WINHTTP_OPTION_CLIENT_CERT_ISSUER_LIST,
            &pIssuerList,
            &dwBufferSize
        ))
        {
            PCERT_CONTEXT pClientCert{ nullptr };
            PCCERT_CHAIN_CONTEXT pClientCertChain{ nullptr };

            CERT_CHAIN_FIND_BY_ISSUER_PARA searchCriteria{};
            searchCriteria.cbSize = sizeof(CERT_CHAIN_FIND_BY_ISSUER_PARA);
            searchCriteria.cIssuer = pIssuerList->cIssuers;
            searchCriteria.rgIssuer = pIssuerList->aIssuers;

            HCERTSTORE hCertStore = CertOpenSystemStore(0, L"MY");
            if (hCertStore)
            {
                pClientCertChain = CertFindChainInStore(
                    hCertStore,
                    X509_ASN_ENCODING,
                    CERT_CHAIN_FIND_BY_ISSUER_CACHE_ONLY_URL_FLAG | CERT_CHAIN_FIND_BY_ISSUER_CACHE_ONLY_FLAG,
                    CERT_CHAIN_FIND_BY_ISSUER,
                    &searchCriteria,
                    nullptr
                );

                if (pClientCertChain)
                {
                    pClientCert = (PCERT_CONTEXT)pClientCertChain->rgpChain[0]->rgpElement[0]->pCertContext;

                    // "!!" to cast from BOOL to bool
                    reissueSend = !!WinHttpSetOption(
                        hRequestHandle,
                        WINHTTP_OPTION_CLIENT_CERT_CONTEXT,
                        (LPVOID)pClientCert,
                        sizeof(CERT_CONTEXT)
                    );

                    CertFreeCertificateChain(pClientCertChain);
                }
                CertCloseStore(hCertStore, 0);
            }
            GlobalFree(pIssuerList);
        }
        else
        {
            auto certError = GetLastError();
            HC_TRACE_ERROR(HTTPCLIENT, "WinHttp returned ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED but unable to get cert issuer list, dwError=%d", certError);
        }
    }

    if (reissueSend)
    {
        const char* url{ nullptr };
        const char* method{ nullptr };
        HRESULT hr = HCHttpCallRequestGetUrl(pRequestContext->m_call, &method, &url);
        if (SUCCEEDED(hr))
        {
            hr = pRequestContext->send(xbox::httpclient::Uri{ url }, method);
            if (FAILED(hr))
            {
                HC_TRACE_ERROR(HTTPCLIENT, "winhttp_http_task Failure to send HTTP request 0x%0.8x", hr);
                pRequestContext->complete_task(E_FAIL, hr);
            }
        }
    }
    else
    {
#if HC_WINHTTP_WEBSOCKETS
        if (pRequestContext->m_isWebSocket)
        {
            pRequestContext->m_socketState = WinHttpWebsockState::Closed;

            if (pRequestContext->m_asyncBlock == nullptr)
            {
                pRequestContext->on_websocket_disconnected(static_cast<USHORT>(errorCode));
            }
        }
#endif
        pRequestContext->complete_task(E_FAIL, errorCode);
    }
}

void winhttp_http_task::callback_status_sendrequest_complete(
    _In_ HINTERNET hRequestHandle,
    _In_ winhttp_http_task* pRequestContext,
    _In_ void* /*statusInfo*/)
{
    HC_TRACE_INFORMATION(HTTPCLIENT, "HCHttpCallPerform [ID %llu] [TID %ul] WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE", HCHttpCallGetId(pRequestContext->m_call), GetCurrentThreadId() );

    if (pRequestContext->m_requestBodyType == content_length_chunked)
    {
        _multiple_segment_write_data(pRequestContext);
    }
    else
    {
        if (!WinHttpReceiveResponse(hRequestHandle, nullptr))
        {
            DWORD dwError = GetLastError();
            HC_TRACE_ERROR(HTTPCLIENT, "HCHttpCallPerform [ID %llu] [TID %ul] WinHttpReceiveResponse errorcode %d", HCHttpCallGetId(pRequestContext->m_call), GetCurrentThreadId(), dwError);
            pRequestContext->complete_task(E_FAIL, HRESULT_FROM_WIN32(dwError));
            return;
        }
    }
}

HRESULT winhttp_http_task::query_header_length(
    _In_ HCCallHandle call,
    _In_ HINTERNET hRequestHandle,
    _In_ DWORD header,
    _Out_ DWORD* pLength)
{
    if (!WinHttpQueryHeaders(
        hRequestHandle,
        header,
        WINHTTP_HEADER_NAME_BY_INDEX,
        WINHTTP_NO_OUTPUT_BUFFER,
        pLength,
        WINHTTP_NO_HEADER_INDEX))
    {
        DWORD dwError = GetLastError();
        if (dwError != ERROR_INSUFFICIENT_BUFFER)
        {
            HC_TRACE_ERROR(HTTPCLIENT, "HCHttpCallPerform [ID %llu] [TID %ul] WinHttpQueryHeaders errorcode %d", HCHttpCallGetId(call), GetCurrentThreadId(), dwError);
            return E_FAIL;
        }
    }

    return S_OK;
}

uint32_t winhttp_http_task::parse_status_code(
    _In_ HCCallHandle call,
    _In_ HINTERNET hRequestHandle,
    _In_ winhttp_http_task* pRequestContext
    )
{
    DWORD length = 0;
    HRESULT hr = query_header_length(pRequestContext->m_call, hRequestHandle, WINHTTP_QUERY_STATUS_CODE, &length);
    if (FAILED(hr))
    {
        pRequestContext->complete_task(E_FAIL, hr);
        return 0;
    }

    http_internal_wstring buffer;
    buffer.resize(length);

    if (!WinHttpQueryHeaders(
        hRequestHandle,
        WINHTTP_QUERY_STATUS_CODE,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &buffer[0],
        &length,
        WINHTTP_NO_HEADER_INDEX))
    {
        DWORD dwError = GetLastError();
        HC_TRACE_ERROR(HTTPCLIENT, "HCHttpCallPerform [ID %llu] [TID %ul] WinHttpQueryHeaders errorcode %d", HCHttpCallGetId(pRequestContext->m_call), GetCurrentThreadId(), dwError);
        pRequestContext->complete_task(E_FAIL, HRESULT_FROM_WIN32(dwError));
        return 0;
    }

    uint32_t statusCode = static_cast<uint32_t>(_wtoi(buffer.c_str()));
    HCHttpCallResponseSetStatusCode(call, statusCode);

    return statusCode;
}


void winhttp_http_task::parse_headers_string(
    _In_ HCCallHandle call,
    _In_ wchar_t* headersStr)
{
    wchar_t* context = nullptr;
    wchar_t* line = wcstok_s(headersStr, CRLF, &context);
    while (line != nullptr)
    {
        http_internal_wstring header_line(line);
        const size_t colonIndex = header_line.find_first_of(L":");
        if (colonIndex != http_internal_wstring::npos)
        {
            http_internal_wstring key = header_line.substr(0, colonIndex);
            http_internal_wstring value = header_line.substr(colonIndex + 1, header_line.length() - colonIndex - 1);
            trim_whitespace(key);
            trim_whitespace(value);

            http_internal_string aKey = utf8_from_utf16(key);
            http_internal_string aValue = utf8_from_utf16(value);
            HCHttpCallResponseSetHeader(call, aKey.c_str(), aValue.c_str());
        }
        line = wcstok_s(nullptr, CRLF, &context);
    }
}

void winhttp_http_task::callback_status_headers_available(
    _In_ HINTERNET hRequestHandle,
    _In_ winhttp_http_task* pRequestContext,
    _In_ void* /*statusInfo*/)
{
    HC_TRACE_INFORMATION(HTTPCLIENT, "winhttp_http_task [ID %llu] [TID %ul] WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE", HCHttpCallGetId(pRequestContext->m_call), GetCurrentThreadId() );

    // First need to query to see what the headers size is.
    DWORD headerBufferLength = 0;
    HRESULT hr = query_header_length(pRequestContext->m_call, hRequestHandle, WINHTTP_QUERY_RAW_HEADERS_CRLF, &headerBufferLength);
    if (FAILED(hr))
    {
        pRequestContext->complete_task(E_FAIL, hr);
        return;
    }

    // Now allocate buffer for headers and query for them.
    http_internal_vector<unsigned char> header_raw_buffer;
    header_raw_buffer.resize(headerBufferLength);
    wchar_t* headerBuffer = reinterpret_cast<wchar_t*>(&header_raw_buffer[0]);
    if (!WinHttpQueryHeaders(
        hRequestHandle,
        WINHTTP_QUERY_RAW_HEADERS_CRLF,
        WINHTTP_HEADER_NAME_BY_INDEX,
        headerBuffer,
        &headerBufferLength,
        WINHTTP_NO_HEADER_INDEX))
    {
        DWORD dwError = GetLastError();
        HC_TRACE_ERROR(HTTPCLIENT, "winhttp_http_task [ID %llu] [TID %ul] WinHttpQueryHeaders errorcode %d", HCHttpCallGetId(pRequestContext->m_call), GetCurrentThreadId(), dwError);
        pRequestContext->complete_task(E_FAIL, HRESULT_FROM_WIN32(dwError));
        return;
    }

    parse_status_code(pRequestContext->m_call, hRequestHandle, pRequestContext);
    parse_headers_string(pRequestContext->m_call, headerBuffer);
    read_next_response_chunk(pRequestContext, 0);
}

void winhttp_http_task::callback_status_data_available(
    _In_ HINTERNET hRequestHandle,
    _In_ winhttp_http_task* pRequestContext,
    _In_ void* statusInfo)
{
    // Status information contains pointer to DWORD containing number of bytes available.
    DWORD newBytesAvailable = *(PDWORD)statusInfo;

    HC_TRACE_INFORMATION(HTTPCLIENT, "winhttp_http_task [ID %llu] [TID %ul] WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE newBytesAvailable=%d", HCHttpCallGetId(pRequestContext->m_call), GetCurrentThreadId(), newBytesAvailable);

    if (newBytesAvailable > 0)
    {
        size_t oldSize = pRequestContext->m_responseBuffer.size();
        size_t newSize = oldSize + newBytesAvailable;
        pRequestContext->m_responseBuffer.resize(newSize);

        // Read in body all at once.
        if (!WinHttpReadData(
            hRequestHandle,
            &pRequestContext->m_responseBuffer[oldSize],
            newBytesAvailable,
            nullptr))
        {
            DWORD dwError = GetLastError();
            HC_TRACE_ERROR(HTTPCLIENT, "winhttp_http_task [ID %llu] [TID %ul] WinHttpReadData errorcode %d", HCHttpCallGetId(pRequestContext->m_call), GetCurrentThreadId(), GetLastError());
            pRequestContext->complete_task(E_FAIL, HRESULT_FROM_WIN32(dwError));
            return;
        }
    }
    else
    {
        // No more data available, complete the request.
        {
            if (pRequestContext->m_responseBuffer.size() > 0)
            {
                HCHttpCallResponseSetResponseBodyBytes(pRequestContext->m_call,
                    pRequestContext->m_responseBuffer.data(),
                    pRequestContext->m_responseBuffer.size()
                );
            }
        }

        pRequestContext->complete_task(S_OK);
    }
}


void winhttp_http_task::callback_status_read_complete(
    _In_ HINTERNET /*hRequestHandle*/,
    _In_ winhttp_http_task* pRequestContext,
    _In_ DWORD statusInfoLength)
{
    // Status information length contains the number of bytes read.
    const DWORD bytesRead = statusInfoLength;

    HC_TRACE_INFORMATION(HTTPCLIENT, "winhttp_http_task [ID %llu] [TID %ul] WINHTTP_CALLBACK_STATUS_READ_COMPLETE bytesRead=%d", HCHttpCallGetId(pRequestContext->m_call), GetCurrentThreadId(), bytesRead);

    // If no bytes have been read, then this is the end of the response.
    if (bytesRead == 0)
    {
        if (pRequestContext->m_responseBuffer.size() > 0)
        {
            HCHttpCallResponseSetResponseBodyBytes(pRequestContext->m_call,
                pRequestContext->m_responseBuffer.data(),
                pRequestContext->m_responseBuffer.size()
            );
        }
        pRequestContext->complete_task(S_OK);
    }
    else
    {
        read_next_response_chunk(pRequestContext, bytesRead);
    }
}

void CALLBACK winhttp_http_task::completion_callback(
    HINTERNET hRequestHandle,
    DWORD_PTR context,
    DWORD statusCode,
    _In_ void* statusInfo,
    DWORD statusInfoLength)
{
    // Callback used with WinHTTP to listen for async completions.
    UNREFERENCED_PARAMETER(statusInfoLength);
    if (statusCode == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING)
        return;

    // Fetch std::shared_ptr if its not removed from cache. 
    // If its removed from cache, then error happened and we can ignore incoming calls
    auto requestContext = shared_ptr_cache::fetch<winhttp_http_task>(reinterpret_cast<void*>(context));
    if (requestContext == nullptr)
    {
        return;
    }

    try
    {
        // The std::shared_ptr of requestContext will keep the object alive during this function 
        // even if its removed from shared_ptr_cache
        winhttp_http_task* pRequestContext = requestContext.get();

        // Process 1 thread at a time since updating shared state
        win32_cs_autolock autoCriticalSection(&pRequestContext->m_lock);

        // Exit early if error happened and it was removed from cache to avoid calling XAsyncComplete() multiple times
        if (shared_ptr_cache::fetch<winhttp_http_task>(reinterpret_cast<void*>(context)) == nullptr)
        {
            return;
        }

        switch (statusCode)
        {
            case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
            {
                callback_status_request_error(hRequestHandle, pRequestContext, statusInfo);
                break;
            }

            case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
            {
                callback_status_sendrequest_complete(hRequestHandle, pRequestContext, statusInfo);
                break;
            }

            case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
            {
                if (pRequestContext->m_isWebSocket)
                {
#if HC_WINHTTP_WEBSOCKETS
                    callback_websocket_status_headers_available(hRequestHandle, pRequestContext);
#endif
                }
                else
                {
                    callback_status_headers_available(hRequestHandle, pRequestContext, statusInfo);
                }
                break;
            }

            case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
            {
                callback_status_data_available(hRequestHandle, pRequestContext, statusInfo);
                break;
            }

            case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
            {
                if (pRequestContext->m_isWebSocket)
                {
#if HC_WINHTTP_WEBSOCKETS
                    callback_websocket_status_read_complete(pRequestContext, statusInfo);
#endif
                }
                else
                {
                    callback_status_read_complete(hRequestHandle, pRequestContext, statusInfoLength);
                }
                break;
            }

            case WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE:
            {
                if (pRequestContext->m_isWebSocket)
                {
#if HC_WINHTTP_WEBSOCKETS
                    SetEvent(pRequestContext->m_hWebsocketWriteComplete);
#endif
                }
                else
                {
                    callback_status_write_complete(hRequestHandle, pRequestContext, statusInfo);
                }
                break;
            }

            case WINHTTP_CALLBACK_STATUS_CLOSE_COMPLETE:
            {
#if HC_WINHTTP_WEBSOCKETS
                USHORT closeReason = 0;
                DWORD dwReasonLengthConsumed = 0;
                WinHttpWebSocketQueryCloseStatus(pRequestContext->m_hRequest, &closeReason, nullptr, 0, &dwReasonLengthConsumed);

                pRequestContext->on_websocket_disconnected(closeReason);
#endif
                break;
            }
        }
    }
    catch (std::bad_alloc const& e)
    {
        HC_TRACE_ERROR(HTTPCLIENT, "winhttp_http_task [%d] std::bad_alloc in completion_callback: %s", E_OUTOFMEMORY, e.what());
        requestContext->complete_task(E_OUTOFMEMORY);
    }
    catch (std::exception const& e)
    {
        HC_TRACE_ERROR(HTTPCLIENT, "winhttp_http_task [%d] std::exception in completion_callback: %s", E_FAIL, e.what());
        requestContext->complete_task(E_FAIL);
    }
    catch (...)
    {
        HC_TRACE_ERROR(HTTPCLIENT, "winhttp_http_task [%d] unknown exception in completion_callback", E_FAIL);
        requestContext->complete_task(E_FAIL);
    }
}

#if HC_PLATFORM != HC_PLATFORM_GSDK
void winhttp_http_task::set_autodiscover_proxy(
    _In_ const xbox::httpclient::Uri& cUri)
{
    WINHTTP_PROXY_INFO info = { 0 };

    WINHTTP_AUTOPROXY_OPTIONS autoproxy_options;
    memset(&autoproxy_options, 0, sizeof(WINHTTP_AUTOPROXY_OPTIONS));
    memset(&info, 0, sizeof(WINHTTP_PROXY_INFO));

    autoproxy_options.dwFlags = WINHTTP_AUTOPROXY_AUTO_DETECT;
    autoproxy_options.dwAutoDetectFlags = WINHTTP_AUTO_DETECT_TYPE_DHCP | WINHTTP_AUTO_DETECT_TYPE_DNS_A;
    autoproxy_options.fAutoLogonIfChallenged = TRUE;

    auto result = WinHttpGetProxyForUrl(
        m_hSession,
        utf16_from_utf8(cUri.FullPath()).c_str(),
        &autoproxy_options,
        &info);
    if (result)
    {
        result = WinHttpSetOption(
            m_hRequest,
            WINHTTP_OPTION_PROXY,
            &info,
            sizeof(WINHTTP_PROXY_INFO));
        if (!result)
        {
            DWORD dwError = GetLastError();
            HC_TRACE_ERROR(HTTPCLIENT, "winhttp_http_task [ID %llu] [TID %ul] WinHttpSetOption errorcode %d", HCHttpCallGetId(m_call), GetCurrentThreadId(), dwError);
        }
    }
    else
    {
        // Failure to download the auto-configuration script is not fatal. Fall back to the default proxy.
    }
}
#endif

http_internal_wstring flatten_http_headers(_In_ const http_header_map& headers)
{
    http_internal_wstring flattened_headers;

    bool foundUserAgent = false;
    for (const auto& header : headers)
    {
        auto wHeaderName = utf16_from_utf8(header.first);
        if (wHeaderName == L"User-Agent")
        {
            foundUserAgent = true;
        }

        flattened_headers.append(wHeaderName);
        flattened_headers.push_back(L':');
        flattened_headers.append(utf16_from_utf8(header.second));
        flattened_headers.append(CRLF);
    }

    if (!foundUserAgent)
    {
        flattened_headers.append(L"User-Agent:libHttpClient/1.0.0.0\r\n");
    }

    return flattened_headers;
}

HRESULT winhttp_http_task::connect(
    _In_ const xbox::httpclient::Uri& cUri
    )
{
    if (!m_hSession)
    {
        HC_TRACE_ERROR(HTTPCLIENT, "winhttp_http_task [ID %llu] [TID %ul] no session", HCHttpCallGetId(m_call), GetCurrentThreadId());
        return E_INVALIDARG;
    }

    if (!m_isWebSocket)
    {
        uint32_t timeoutInSeconds = 0;
        HRESULT hr = HCHttpCallRequestGetTimeout(m_call, &timeoutInSeconds);
        if (FAILED(hr))
        {
            return hr;
        }

        int timeoutInMilliseconds = static_cast<int>(timeoutInSeconds * 1000);
        if (!WinHttpSetTimeouts(
            m_hSession,
            timeoutInMilliseconds,
            timeoutInMilliseconds,
            timeoutInMilliseconds,
            timeoutInMilliseconds))
        {
            DWORD dwError = GetLastError();
            HC_TRACE_ERROR(HTTPCLIENT, "winhttp_http_task [ID %llu] [TID %ul] WinHttpSetTimeouts errorcode %d", HCHttpCallGetId(m_call), GetCurrentThreadId(), dwError);
            return HRESULT_FROM_WIN32(dwError);
        }
    }
 
    m_isSecure = cUri.IsSecure();
    unsigned int port = cUri.IsPortDefault() ?
        (m_isSecure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT) :
        cUri.Port();
    http_internal_wstring wUrlHost = utf16_from_utf8(cUri.Host());

    m_hConnection = WinHttpConnect(
        m_hSession,
        wUrlHost.c_str(),
        (INTERNET_PORT)port,
        0);
    if (m_hConnection == nullptr)
    {
        DWORD dwError = GetLastError();
        HC_TRACE_ERROR(HTTPCLIENT, "winhttp_http_task [ID %llu] [TID %ul] WinHttpConnect errorcode %d", HCHttpCallGetId(m_call), GetCurrentThreadId(), dwError);
        return HRESULT_FROM_WIN32(dwError);
    }

    return S_OK;
}

HRESULT winhttp_http_task::send(
    _In_ const xbox::httpclient::Uri& cUri,
    _In_ const char* method)
{
    // Need to form uri path, query, and fragment for this request.
    http_internal_wstring wEncodedResource = utf16_from_utf8(cUri.Resource());
    http_internal_wstring wMethod = utf16_from_utf8(method);

    // Open the request.
    m_hRequest = WinHttpOpenRequest(
        m_hConnection,
        wMethod.c_str(),
        wEncodedResource.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_ESCAPE_DISABLE | (cUri.IsSecure() ? WINHTTP_FLAG_SECURE : 0));
    if (m_hRequest == nullptr)
    {
        DWORD dwError = GetLastError();
        HC_TRACE_ERROR(HTTPCLIENT, "winhttp_http_task [ID %llu] [TID %ul] WinHttpOpenRequest errorcode %d", HCHttpCallGetId(m_call), GetCurrentThreadId(), dwError);
        return HRESULT_FROM_WIN32(dwError);
    }

#if HC_PLATFORM != HC_PLATFORM_GSDK
    if (m_proxyType == proxy_type::autodiscover_proxy)
    {
        set_autodiscover_proxy(cUri);
    }
#endif

    const BYTE* requestBody = nullptr;
    uint32_t requestBodyBytes = 0;
    HRESULT hr = HCHttpCallRequestGetRequestBodyBytes(m_call, &requestBody, &requestBodyBytes);
    if (FAILED(hr))
    {
        return hr;
    }

    if (requestBodyBytes > 0)
    {
        // While we won't be transfer-encoding the data, we will write it in portions.
        m_requestBodyType = msg_body_type::content_length_chunked;
        m_requestBodyRemainingToWrite = requestBodyBytes;
    }
    else
    {
        m_requestBodyType = msg_body_type::no_body;
        m_requestBodyRemainingToWrite = 0;
    }

    uint32_t numHeaders = 0;
    hr = HCHttpCallRequestGetNumHeaders(m_call, &numHeaders);
    if (FAILED(hr))
    {
        return hr;
    }

    if (numHeaders > 0)
    {
        http_internal_wstring flattenedHeaders = flatten_http_headers(m_call->requestHeaders);
        if (!WinHttpAddRequestHeaders(
                m_hRequest,
                flattenedHeaders.c_str(),
                static_cast<DWORD>(flattenedHeaders.length()),
                WINHTTP_ADDREQ_FLAG_ADD))
        {
            DWORD dwError = GetLastError();
            HC_TRACE_ERROR(HTTPCLIENT, "winhttp_http_task [ID %llu] [TID %ul] WinHttpAddRequestHeaders errorcode %d", HCHttpCallGetId(m_call), GetCurrentThreadId(), dwError);
            return HRESULT_FROM_WIN32(dwError);
        }
    }

    if (WINHTTP_INVALID_STATUS_CALLBACK == WinHttpSetStatusCallback(
        m_hRequest,
        &winhttp_http_task::completion_callback,
        WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS,
        0))
    {
        DWORD dwError = GetLastError();
        HC_TRACE_ERROR(HTTPCLIENT, "winhttp_http_task [ID %llu] [TID %ul] WinHttpSetStatusCallback errorcode %d", HCHttpCallGetId(m_call), GetCurrentThreadId(), dwError);
        return HRESULT_FROM_WIN32(dwError);
    }

    if (m_isSecure)
    {
        if (!WinHttpSetOption(
            m_hRequest,
            WINHTTP_OPTION_CLIENT_CERT_CONTEXT,
            WINHTTP_NO_CLIENT_CERT_CONTEXT,
            0))
        {
            DWORD dwError = GetLastError();
            HC_TRACE_ERROR(HTTPCLIENT, "winhttp_http_task [ID %llu] [TID %ul] WinHttpSetOption errorcode %d", HCHttpCallGetId(m_call), GetCurrentThreadId(), dwError);
            return HRESULT_FROM_WIN32(dwError);
        }
    }

#if HC_WINHTTP_WEBSOCKETS
    if (m_isWebSocket)
    {
        if (!m_websocketHandle->Headers().empty())
        {
            http_internal_wstring flattenedHeaders = flatten_http_headers(m_websocketHandle->Headers());
            if (!WinHttpAddRequestHeaders(
                m_hRequest,
                flattenedHeaders.c_str(),
                static_cast<DWORD>(flattenedHeaders.length()),
                WINHTTP_ADDREQ_FLAG_ADD))
            {
                DWORD dwError = GetLastError();
                HC_TRACE_ERROR(HTTPCLIENT, "winhttp_http_task [ID %llu] [TID %ul] WinHttpAddRequestHeaders errorcode %d", HCHttpCallGetId(m_call), GetCurrentThreadId(), dwError);
                return HRESULT_FROM_WIN32(dwError);
            }
        }

        // Request protocol upgrade from http to websocket.
        #pragma prefast(suppress:6387, "WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET does not take any arguments.")

        bool status = WinHttpSetOption(m_hRequest, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0);
        if (!status)
        {
            DWORD dwError = GetLastError();
            HC_TRACE_ERROR(HTTPCLIENT, "winhttp_http_task [ID %llu] [TID %ul] WinHttpAddRequestHeaders errorcode %d", HCHttpCallGetId(m_call), GetCurrentThreadId(), dwError);
            return HRESULT_FROM_WIN32(dwError);
        }
    }
#endif

    DWORD dwTotalLength = 0;
    switch (m_requestBodyType)
    {
        case msg_body_type::no_body: dwTotalLength = 0; break;
        case msg_body_type::content_length_chunked: dwTotalLength = (DWORD)requestBodyBytes; break;
        default: dwTotalLength = WINHTTP_IGNORE_REQUEST_TOTAL_LENGTH; break;
    }

    if (!WinHttpSendRequest(
        m_hRequest,
        WINHTTP_NO_ADDITIONAL_HEADERS,
        0,
        nullptr,
        0,
        dwTotalLength,
        (DWORD_PTR)this))
    {
        DWORD dwError = GetLastError();
        HC_TRACE_ERROR(HTTPCLIENT, "winhttp_http_task [ID %llu] [TID %ul] WinHttpSendRequest errorcode %d", HCHttpCallGetId(m_call), GetCurrentThreadId(), dwError);
        return HRESULT_FROM_WIN32(dwError);
    }

    return S_OK;
}


HRESULT winhttp_http_task::connect_and_send_async()
{
    try
    {
        const char* url = nullptr;
        const char* method = nullptr;
        HRESULT hr = HCHttpCallRequestGetUrl(m_call, &method, &url);
        if (SUCCEEDED(hr))
        {
            xbox::httpclient::Uri cUri(url);

            hr = connect(cUri);
            if (SUCCEEDED(hr))
            {
                hr = send(cUri, method);
            }
        }

        if (FAILED(hr))
        {
            HC_TRACE_ERROR(HTTPCLIENT, "winhttp_http_task Failure to send HTTP request 0x%0.8x", hr);
            complete_task(E_FAIL, hr);
            return hr;
        }
    }
    catch (std::bad_alloc const& e)
    {
        HC_TRACE_ERROR(HTTPCLIENT, "winhttp_http_task [%d] std::bad_alloc: %s", E_OUTOFMEMORY, e.what());
        complete_task(E_OUTOFMEMORY);
    }
    catch (std::exception const& e)
    {
        HC_TRACE_ERROR(HTTPCLIENT, "winhttp_http_task [%d] std::exception: %s", E_FAIL, e.what());
        complete_task(E_FAIL);
    }
    catch (...)
    {
        HC_TRACE_ERROR(HTTPCLIENT, "winhttp_http_task [%d] unknown exception", E_FAIL);
        complete_task(E_FAIL);
    }

    return S_OK;
}

NAMESPACE_XBOX_HTTP_CLIENT_END

#if HC_PLATFORM == HC_PLATFORM_GSDK
typedef DWORD(WINAPI *GetNetworkConnectivityHintProc)(NL_NETWORK_CONNECTIVITY_HINT*);
typedef DWORD(WINAPI *NotifyNetworkConnectivityHintChangeProc)(PNETWORK_CONNECTIVITY_HINT_CHANGE_CALLBACK, PVOID, BOOLEAN, PHANDLE);

static void NetworkConnectivityHintChangedCallback(
    _In_ void* context,
    _In_ NL_NETWORK_CONNECTIVITY_HINT connectivityHint
)
{
    UNREFERENCED_PARAMETER(context);

    auto singleton = get_http_singleton(false);

    if (singleton != nullptr)
    {
        singleton->m_networkInitialized = connectivityHint.ConnectivityLevel != NetworkConnectivityLevelHintUnknown;
    }
}
#endif

HRESULT Internal_InitializeHttpPlatform(HCInitArgs* args, PerformEnv& performEnv) noexcept
{
    assert(args == nullptr);
    UNREFERENCED_PARAMETER(args);

    performEnv.reset(new (std::nothrow) HC_PERFORM_ENV());
    if (!performEnv) { return E_OUTOFMEMORY; }

    return S_OK;
}

void Internal_CleanupHttpPlatform(HC_PERFORM_ENV* performEnv) noexcept
{
#if HC_PLATFORM == HC_PLATFORM_GSDK
    auto singleton = get_http_singleton(false);
    if (singleton != nullptr && singleton->m_networkModule != nullptr)
    {
        FreeLibrary(singleton->m_networkModule);
        singleton->m_networkModule = nullptr;
    }
#endif

    delete performEnv;
}

HRESULT
Internal_SetGlobalProxy(
    _In_ HC_PERFORM_ENV* performEnv,
    _In_ const char* proxyUri) noexcept
{
    assert(performEnv != nullptr);

    auto desiredType = xbox::httpclient::proxy_type::default_proxy;
    WINHTTP_PROXY_INFO info = { 0 };
    http_internal_wstring wProxyName;

    if (proxyUri == nullptr)
    {
        HC_TRACE_INFORMATION(HTTPCLIENT, "Internal_SetGlobalProxy [TID %ul] reseting proxy", GetCurrentThreadId());

        xbox::httpclient::Uri ieProxyUri;
        desiredType = get_ie_proxy_info(proxy_protocol::https, ieProxyUri);

        DWORD accessType = WINHTTP_ACCESS_TYPE_DEFAULT_PROXY;
        get_proxy_name(desiredType, ieProxyUri, &accessType, &wProxyName);

        info.dwAccessType = accessType;
        if (wProxyName.length() > 0)
        {
            info.lpszProxy = const_cast<LPWSTR>(wProxyName.c_str());
        }
    }
    else
    {
        HC_TRACE_INFORMATION(HTTPCLIENT, "Internal_SetGlobalProxy [TID %ul] setting proxy to '%s'", GetCurrentThreadId(), proxyUri);

        wProxyName = utf16_from_utf8(proxyUri);

        info.dwAccessType = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
        info.lpszProxy = const_cast<LPWSTR>(wProxyName.c_str());

        desiredType = xbox::httpclient::proxy_type::named_proxy;
    }

    auto result = WinHttpSetOption(
        performEnv->m_hSession,
        WINHTTP_OPTION_PROXY,
        &info,
        sizeof(WINHTTP_PROXY_INFO));
    if (!result)
    {
        DWORD dwError = GetLastError();
        HC_TRACE_ERROR(HTTPCLIENT, "Internal_SetGlobalProxy [TID %ul] WinHttpSetOption errorcode %d", GetCurrentThreadId(), dwError);
        return E_FAIL;
    }
    else
    {
        performEnv->m_proxyType = desiredType;
    }

    return S_OK;
}

void CALLBACK Internal_HCHttpCallPerformAsync(
    _In_ HCCallHandle call,
    _Inout_ XAsyncBlock* asyncBlock,
    _In_opt_ void* context,
    _In_ HCPerformEnv env
    ) noexcept
{
    assert(env != nullptr);
    UNREFERENCED_PARAMETER(context);


#if HC_PLATFORM == HC_PLATFORM_GSDK 
    if (XGameRuntimeIsFeatureAvailable(XGameRuntimeFeature::XNetworking))
    {
        auto singleton = get_http_singleton(true);
        if (singleton != nullptr)
        {
            if (singleton->m_networkModule == nullptr)
            {
                singleton->m_networkModule = LoadLibrary(TEXT("iphlpapi.dll"));

                if (singleton->m_networkModule != nullptr)
                {
                    GetNetworkConnectivityHintProc getNetworkConnectivityHint =
                        (GetNetworkConnectivityHintProc)GetProcAddress(singleton->m_networkModule, "GetNetworkConnectivityHint");

                    NotifyNetworkConnectivityHintChangeProc notifyNetworkConnectivityHintChange =
                        (NotifyNetworkConnectivityHintChangeProc)GetProcAddress(singleton->m_networkModule, "NotifyNetworkConnectivityHintChange");

                    if (getNetworkConnectivityHint != nullptr && notifyNetworkConnectivityHintChange != nullptr)
                    {
                        NL_NETWORK_CONNECTIVITY_HINT connectivityHint{};
                        HRESULT hr = HRESULT_FROM_WIN32(getNetworkConnectivityHint(&connectivityHint));
                        singleton->m_networkInitialized = SUCCEEDED(hr) && connectivityHint.ConnectivityLevel != NetworkConnectivityLevelHintUnknown;

                        HANDLE networkConnectivityChangedHandle;
                        std::weak_ptr<http_singleton> singletonWeakPtr = singleton;
                        (void)HRESULT_FROM_WIN32(notifyNetworkConnectivityHintChange(
                            NetworkConnectivityHintChangedCallback,
                            nullptr,
                            TRUE,
                            &networkConnectivityChangedHandle));
                    }
                }
            }

            if (!singleton->m_networkInitialized)
            {
                XAsyncComplete(asyncBlock, E_HC_NETWORK_NOT_INITIALIZED, 0);
                return;
            }
        }
    }
#endif

    bool isWebsocket = false;
    std::shared_ptr<xbox::httpclient::winhttp_http_task> httpTask = http_allocate_shared<winhttp_http_task>(
        asyncBlock, call, env->m_hSession, env->m_proxyType, isWebsocket);
    auto raw = shared_ptr_cache::store<winhttp_http_task>(httpTask);
    if (raw == nullptr)
    {
        XAsyncComplete(asyncBlock, E_HC_NOT_INITIALISED, 0);
        return;
    }

    HCHttpCallSetContext(call, httpTask.get());
    httpTask->connect_and_send_async();
}



#if HC_WINHTTP_WEBSOCKETS
HRESULT winhttp_http_task::send_websocket_message(
    WINHTTP_WEB_SOCKET_BUFFER_TYPE eBufferType,
    _In_ const void* payloadPtr,
    _In_ size_t payloadLength)
{
    DWORD dwError = WinHttpWebSocketSend(m_hRequest,
        eBufferType, 
        (PVOID)payloadPtr,
        static_cast<DWORD>(payloadLength));
    if (FAILED(HRESULT_FROM_WIN32(dwError)))
    {
        return HRESULT_FROM_WIN32(dwError);
    }

    // In WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE, it will set m_hWebsocketWriteComplete
    DWORD dwResult = WaitForSingleObject(m_hWebsocketWriteComplete, 30000);
    if (dwResult == WAIT_TIMEOUT)
    {
        HC_TRACE_ERROR(HTTPCLIENT, "winhttp_http_task [ID %llu] [TID %ul] write complete timeout", HCHttpCallGetId(m_call), GetCurrentThreadId());
        return E_FAIL;
    }

    return S_OK;
}

HRESULT winhttp_http_task::on_websocket_disconnected(_In_ USHORT closeReason)
{
    m_socketState = WinHttpWebsockState::Closed;

    // Handlers will be setup again upon connect
    WinHttpSetStatusCallback(m_hRequest, nullptr, WINHTTP_CALLBACK_FLAG_ALL_NOTIFICATIONS, NULL); 

    HCWebSocketCloseEventFunction disconnectFunc = nullptr;
    void* functionContext = nullptr;
    HCWebSocketGetEventFunctions(m_websocketHandle, nullptr, nullptr, &disconnectFunc, &functionContext);

    try
    {
        HCWebSocketCloseStatus closeStatus = static_cast<HCWebSocketCloseStatus>(closeReason); 
        disconnectFunc(m_websocketHandle, closeStatus, functionContext);
    }
    catch (...)
    {
    }

    return S_OK;
}

HRESULT winhttp_http_task::disconnect_websocket(_In_ HCWebSocketCloseStatus closeStatus)
{
    m_socketState = WinHttpWebsockState::Closed;
    DWORD dwError = WinHttpWebSocketShutdown(m_hRequest, static_cast<short>(closeStatus), nullptr, 0);

    return HRESULT_FROM_WIN32(dwError);
}

char* winhttp_http_task::winhttp_web_socket_buffer_type_to_string(
    _In_ WINHTTP_WEB_SOCKET_BUFFER_TYPE bufferType
)
{
    switch (bufferType)
    {
        case WINHTTP_WEB_SOCKET_BUFFER_TYPE::WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE: return "WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE";
        case WINHTTP_WEB_SOCKET_BUFFER_TYPE::WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE: return "WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE";
        case WINHTTP_WEB_SOCKET_BUFFER_TYPE::WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE: return "WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE";
        case WINHTTP_WEB_SOCKET_BUFFER_TYPE::WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE: return "WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE";
        case WINHTTP_WEB_SOCKET_BUFFER_TYPE::WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE: return "WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE";
    }
    return "unknown";
}

void winhttp_http_task::callback_websocket_status_read_complete(
    _In_ winhttp_http_task* pRequestContext,
    _In_ void* statusInfo)
{
    WINHTTP_WEB_SOCKET_STATUS* wsStatus = static_cast<WINHTTP_WEB_SOCKET_STATUS*>(statusInfo);
    if (wsStatus == nullptr)
    {
        return;
    }

    HC_TRACE_VERBOSE(WEBSOCKET, "[WinHttp] callback_websocket_status_read_complete: buffer type %s", winhttp_web_socket_buffer_type_to_string(wsStatus->eBufferType));
    if (wsStatus->eBufferType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
    {
        USHORT closeReason = 0;
        DWORD dwReasonLengthConsumed = 0;
        WinHttpWebSocketQueryCloseStatus(pRequestContext->m_hRequest, &closeReason, nullptr, 0, &dwReasonLengthConsumed);

        pRequestContext->on_websocket_disconnected(closeReason);
    }
    else if (wsStatus->eBufferType == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE || wsStatus->eBufferType == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE)
    {
        pRequestContext->m_websocketResponseBuffer.FinishWriteData(wsStatus->dwBytesTransferred);
        pRequestContext->websocket_read_message();
    }
    else if (wsStatus->eBufferType == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE)
    {
        pRequestContext->m_websocketResponseBuffer.FinishWriteData(wsStatus->dwBytesTransferred);

        websocket_message_buffer responseBuffer;
        pRequestContext->m_websocketResponseBuffer.TransferBuffer(&responseBuffer);

        HCWebSocketMessageFunction messageFunc = nullptr;
        void* functionContext = nullptr;
        HCWebSocketGetEventFunctions(pRequestContext->m_websocketHandle, &messageFunc, nullptr, nullptr, &functionContext);

        if (messageFunc)
        {
            try
            {
                char* buffer = reinterpret_cast<char*>(responseBuffer.GetBuffer());
                uint32_t bufferLength = responseBuffer.GetBufferByteCount();
                buffer[bufferLength] = 0;

                messageFunc(pRequestContext->m_websocketHandle, buffer, functionContext);
            }
            catch (...)
            {
            }
        }

        pRequestContext->websocket_start_listening();
    }
    else if (wsStatus->eBufferType == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE)
    {
        pRequestContext->m_websocketResponseBuffer.FinishWriteData(wsStatus->dwBytesTransferred);

        websocket_message_buffer responseBuffer;
        pRequestContext->m_websocketResponseBuffer.TransferBuffer(&responseBuffer);

        HCWebSocketBinaryMessageFunction messageFunc = nullptr;
        void* functionContext = nullptr;
        HCWebSocketGetEventFunctions(pRequestContext->m_websocketHandle, nullptr, &messageFunc, nullptr, &functionContext);

        if (messageFunc)
        {
            try
            {
                messageFunc(pRequestContext->m_websocketHandle, responseBuffer.GetBuffer(), responseBuffer.GetBufferByteCount(), functionContext);
            }
            catch (...)
            {
            }
        }

        pRequestContext->websocket_start_listening();
    }

}

HRESULT winhttp_http_task::websocket_start_listening()
{
    HC_TRACE_VERBOSE(HTTPCLIENT, "WINHTTP_WEB_SOCKET_UTF8 [ID %llu] [TID %ul] listening", HCHttpCallGetId(m_call), GetCurrentThreadId());
    HRESULT hr = m_websocketResponseBuffer.Resize(WINHTTP_WEBSOCKET_RECVBUFFER_SIZE);
    if (SUCCEEDED(hr))
    {
        hr = websocket_read_message();
    }

    return hr;
}

HRESULT winhttp_http_task::websocket_read_message()
{
    HRESULT hr = S_OK;

    if (m_websocketResponseBuffer.GetRemainingCapacity() == 0)
    {
        if (m_websocketResponseBuffer.GetBufferByteCount() < WINHTTP_WEBSOCKET_RECVBUFFER_MAXSIZE)
        {
            uint32_t newSize = m_websocketResponseBuffer.GetBufferByteCount() * 2;
            if (newSize > WINHTTP_WEBSOCKET_RECVBUFFER_MAXSIZE)
            {
                newSize = WINHTTP_WEBSOCKET_RECVBUFFER_MAXSIZE;
            }

            hr = m_websocketResponseBuffer.Resize(newSize);
        }
        else
        {
            hr = E_ABORT;
        }
    }

    if (SUCCEEDED(hr))
    {
        uint8_t* bufferPtr = m_websocketResponseBuffer.GetNextWriteLocation();
        uint64_t bufferSize = m_websocketResponseBuffer.GetRemainingCapacity();
        DWORD dwError = ERROR_SUCCESS;
        dwError = WinHttpWebSocketReceive(m_hRequest, bufferPtr, (DWORD)bufferSize, nullptr, nullptr);
        if (dwError)
        {
            HC_TRACE_ERROR(HTTPCLIENT, "[WinHttp] websocket_read_message [ID %llu] [TID %ul] errorcode %d", HCHttpCallGetId(m_call), GetCurrentThreadId(), dwError);
        }
    }

    return hr;
}

void winhttp_http_task::callback_websocket_status_headers_available(
    _In_ HINTERNET hRequestHandle,
    _In_ winhttp_http_task* pRequestContext)
{
    HC_TRACE_INFORMATION(HTTPCLIENT, "HCHttpCallPerform [ID %llu] [TID %ul] Websocket WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE", HCHttpCallGetId(pRequestContext->m_call), GetCurrentThreadId());

    // Application should check what is the HTTP status code returned by the server and behave accordingly.
    // WinHttpWebSocketCompleteUpgrade will fail if the HTTP status code is different than 101.
    pRequestContext->m_hRequest = WinHttpWebSocketCompleteUpgrade(hRequestHandle, NULL);
    if (pRequestContext->m_hRequest == NULL)
    {
        DWORD dwError = GetLastError();
        HC_TRACE_ERROR(HTTPCLIENT, "HCHttpCallPerform [ID %llu] [TID %ul] WinHttpWebSocketCompleteUpgrade errorcode %d", HCHttpCallGetId(pRequestContext->m_call), GetCurrentThreadId(), dwError);
        pRequestContext->complete_task(E_FAIL, HRESULT_FROM_WIN32(dwError));
    }

    void* pThis = pRequestContext;
    if (!WinHttpSetOption(pRequestContext->m_hRequest, WINHTTP_OPTION_CONTEXT_VALUE, &pThis, sizeof(pThis)))
    {
        DWORD dwError = GetLastError();
        HC_TRACE_ERROR(HTTPCLIENT, "HCHttpCallPerform [ID %llu] [TID %ul] WinHttpSetOption errorcode %d", HCHttpCallGetId(pRequestContext->m_call), GetCurrentThreadId(), dwError);
        pRequestContext->complete_task(E_FAIL, HRESULT_FROM_WIN32(dwError));
    }

    pRequestContext->websocket_start_listening();
    pRequestContext->m_socketState = WinHttpWebsockState::Connected;

    WinHttpCloseHandle(hRequestHandle); // The old request handle is not needed anymore.  We're using pRequestContext->m_hRequest now

    pRequestContext->complete_task(S_OK, S_OK);
}

#endif

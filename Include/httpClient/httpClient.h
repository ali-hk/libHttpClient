// Copyright (c) Microsoft Corporation
// Licensed under the MIT license. See LICENSE file in the project root for full license information.
#if !defined(__cplusplus)
    #error C++11 required
#endif

#pragma once
#include <httpClient/async.h>
#include <httpClient/mock.h>
#include <httpClient/pal.h>
#include <httpClient/trace.h>

#if HC_PLATFORM == HC_PLATFORM_ANDROID
#include "jni.h"
#endif

extern "C"
{

/////////////////////////////////////////////////////////////////////////////////////////
// Memory APIs
//

/// <summary>
/// A callback invoked every time a new memory buffer must be dynamically allocated by the library.
/// This callback is optionally installed by calling HCMemSetFunctions()
/// 
/// The callback must allocate and return a pointer to a contiguous block of memory of the 
/// specified size that will remain valid until the app's corresponding HCMemFreeFunction 
/// callback is invoked to release it.
/// 
/// Every non-null pointer returned by this method will be subsequently passed to the corresponding
/// HCMemFreeFunction callback once the memory is no longer needed.
/// </summary>
/// <returns>A pointer to an allocated block of memory of the specified size, or a null 
/// pointer if allocation failed.</returns>
/// <param name="size">The size of the allocation to be made. This value will never be zero.</param>
/// <param name="memoryType">An opaque identifier representing the internal category of 
/// memory being allocated.</param>
typedef _Ret_maybenull_ _Post_writable_byte_size_(size) void*
(STDAPIVCALLTYPE* HCMemAllocFunction)(
    _In_ size_t size,
    _In_ HCMemoryType memoryType
    );

/// <summary>
/// A callback invoked every time a previously allocated memory buffer is no longer needed by 
/// the library and can be freed. This callback is optionally installed by calling HCMemSetFunctions()
///
/// The callback is invoked whenever the library has finished using a memory buffer previously 
/// returned by the app's corresponding HCMemAllocFunction such that the application can free the
/// memory buffer.
/// </summary>
/// <param name="pointer">The pointer to the memory buffer previously allocated. This value will
/// never be a null pointer.</param>
/// <param name="memoryType">An opaque identifier representing the internal category of 
/// memory being allocated.</param>
typedef void
(STDAPIVCALLTYPE* HCMemFreeFunction)(
    _In_ _Post_invalid_ void* pointer,
    _In_ HCMemoryType memoryType
    );

/// <summary>
/// Optionally sets the memory hook functions to allow callers to control route memory 
/// allocations to their own memory manager. This must be called before HCInitialize() 
/// and can not be called again until HCCleanup()
///
/// This method allows the application to install custom memory allocation routines in order 
/// to service all requests for new memory buffers instead of using default allocation routines.
///
/// The <paramref name="memAllocFunc" /> and <paramref name="memFreeFunc" /> parameters can be null
/// pointers to restore the default routines. Both callback pointers must be null or both must 
/// be non-null. Mixing custom and default routines is not permitted.
/// </summary>
/// <param name="memAllocFunc">A pointer to the custom allocation callback to use, or a null 
/// pointer to restore the default.</param>
/// <param name="memFreeFunc">A pointer to the custom freeing callback to use, or a null 
/// pointer to restore the default.</param>
/// <returns>Result code for this API operation.  Possible values are S_OK, or E_HC_ALREADY_INITIALIZED.</returns>
STDAPI HCMemSetFunctions(
    _In_opt_ HCMemAllocFunction memAllocFunc,
    _In_opt_ HCMemFreeFunction memFreeFunc
    ) noexcept;

/// <summary>
/// Gets the memory hook functions to allow callers to control route memory allocations to their 
/// own memory manager.  This method allows the application get the default memory allocation routines.
/// This can be used along with HCMemSetFunctions() to monitor all memory allocations.
/// </summary>
/// <param name="memAllocFunc">Set to the current allocation callback.  Returns the default routine 
/// if not previously set</param>
/// <param name="memFreeFunc">Set to the to the current memory free callback.  Returns the default 
/// routine if not previously set</param>
/// <returns>Result code for this API operation.  Possible values are S_OK, E_INVALIDARG, or E_FAIL.</returns>
STDAPI HCMemGetFunctions(
    _Out_ HCMemAllocFunction* memAllocFunc,
    _Out_ HCMemFreeFunction* memFreeFunc
    ) noexcept;


/////////////////////////////////////////////////////////////////////////////////////////
// Global APIs
// 

#if HC_PLATFORM == HC_PLATFORM_ANDROID
/// <summary>
/// Used to wrap the JavaVM and ApplicationContext on Android devices.
/// </summary>
typedef struct HCInitArgs {
    /// <summary>The Java Virtual machine.</summary>
    JavaVM *javaVM;
    /// <summary>The Java Application Context.</summary>
    jobject applicationContext;
} HCInitArgs;
#else 
/// <summary>
/// Dummy init args used by non-Android devices.
/// </summary>
typedef struct HCInitArgs {
    /// <summary>A dummy field.</summary>
    void* dummy;
} HCInitArgs;
#endif

/// <summary>
/// Initializes the library instance.
/// This must be called before any other method, except for HCMemSetFunctions() and HCMemGetFunctions()
/// Should have a corresponding call to HCGlobalCleanup().
/// </summary>
/// <param name="context">Client context for platform-specific initialization.  Pass in the JavaVM on Android, and nullptr on other platforms</param>
/// <returns>Result code for this API operation.  Possible values are S_OK, E_INVALIDARG, E_OUTOFMEMORY, or E_FAIL.</returns>
STDAPI HCInitialize(_In_opt_ HCInitArgs* args) noexcept;

/// <summary>
/// Immediately reclaims all resources associated with the library.
/// If you called HCMemSetFunctions(), call this before shutting down your app's memory manager.
/// </summary>
/// <returns></returns>
STDAPI_(void) HCCleanup() noexcept;

/// <summary>
/// Returns the version of the library
/// </summary>
/// <param name="version">The UTF-8 encoded version of the library in the format of release_year.release_month.date.rev.  
/// For example, 2017.07.20170710.01</param>
/// <returns>Result code for this API operation.  Possible values are S_OK, E_INVALIDARG, or E_FAIL.</returns>
STDAPI HCGetLibVersion(_Outptr_ const char** version) noexcept;

/// <summary>
/// A callback that will be synchronously invoked each time an HTTP call fails but will be automatically be
/// retried. Can be used to track intermittent failures similar to fiddler.
/// </summary>
/// <param name="call">Handle to the HTTP call that failed.</param>
/// <param name="context">Client context pass when the handler was added.</param>
typedef void
(STDAPIVCALLTYPE* HCCallRoutedHandler)(
    _In_ HCCallHandle call,
    _In_opt_ void* context
    );

/// <summary>
/// Adds a callback to be invoked on intermediate http errors (errors that are non-fatal and will
/// automatically be retried).
/// </summary>
/// <param name="handler">The handler to be called.</param>
/// <param name="context">Client context to pass to callback function.</param>
/// <returns>An unique id that can be used to remove the handler.</returns>
STDAPI_(int32_t) HCAddCallRoutedHandler(
    _In_ HCCallRoutedHandler handler,
    _In_opt_ void* context
    ) noexcept;

/// <summary>
/// Removes a previously added HCCallRoutedHandler.
/// </summary>
/// <param name="handlerId">Id returned from the HCAddCallRoutedHandler call.</param>
/// <returns></returns>
STDAPI_(void) HCRemoveCallRoutedHandler(
    _In_ int32_t handlerId
    ) noexcept;

/// <summary>
/// Manually sets an explicit proxy address. If it is passed a null proxy, it will reset
/// to default. Does not include proxying web socket traffic.
/// </summary>
/// <param name="proxyUri">The proxy address to use in the "[ip]:[port]" format</param> 
/// <returns>Result code for this API operation. Possible values are S_OK, E_HC_NOT_INITIALISED, or E_FAIL.</returns>
STDAPI HCSetGlobalProxy(_In_ const char* proxyUri) noexcept;

/////////////////////////////////////////////////////////////////////////////////////////
// Http APIs
//

/// <summary>
/// Creates an HTTP call handle
///
/// First create a HTTP handle using HCHttpCallCreate()
/// Then call HCHttpCallRequestSet*() to prepare the HCCallHandle
/// Then call HCHttpCallPerformAsync() to perform HTTP call using the HCCallHandle.
/// This call is asynchronous, so the work will be done on a background thread and will return via the callback.
///
/// The perform call is asynchronous, so the work will be done on a background thread which calls 
/// XTaskQueueDispatch( ..., XTaskQueuePort::Work ).  
///
/// The results will return to the callback on the thread that calls 
/// XTaskQueueDispatch( ..., XTaskQueuePort::Completion ), then get the result of the HTTP call by calling 
/// HCHttpCallResponseGet*() to get the HTTP response of the HCCallHandle.
/// 
/// When the HCCallHandle is no longer needed, call HCHttpCallCloseHandle() to free the 
/// memory associated with the HCCallHandle
/// </summary>
/// <param name="call">The handle of the HTTP call</param>
/// <returns>Result code for this API operation.  Possible values are S_OK, E_INVALIDARG, or E_FAIL.</returns>
STDAPI HCHttpCallCreate(
    _Out_ HCCallHandle* call
    ) noexcept;

/// <summary>
/// Perform HTTP call using the HCCallHandle
///
/// First create a HTTP handle using HCHttpCallCreate()
/// Then call HCHttpCallRequestSet*() to prepare the HCCallHandle
/// Then call HCHttpCallPerformAsync() to perform HTTP call using the HCCallHandle.
/// This call is asynchronous, so the work will be done on a background thread and will return via the callback.
///
/// The perform call is asynchronous, so the work will be done on a background thread which calls 
/// XTaskQueueDispatch( ..., XTaskQueuePort::Work ).  
///
/// The results will return to the callback on the thread that calls 
/// XTaskQueueDispatch( ..., XTaskQueuePort::Completion ), then get the result of the HTTP call by calling 
/// HCHttpCallResponseGet*() to get the HTTP response of the HCCallHandle.
/// 
/// When the HCCallHandle is no longer needed, call HCHttpCallCloseHandle() to free the 
/// memory associated with the HCCallHandle
///
/// HCHttpCallPerformAsync can only be called once.  Create new HCCallHandle to repeat the call.
/// </summary>
/// <param name="call">The handle of the HTTP call</param>
/// <param name="asyncBlock">The XAsyncBlock that defines the async operation</param>
/// <returns>Result code for this API operation.  Possible values are S_OK, E_INVALIDARG, E_OUTOFMEMORY, or E_FAIL.</returns>
STDAPI HCHttpCallPerformAsync(
    _In_ HCCallHandle call,
    _Inout_ XAsyncBlock* asyncBlock
    ) noexcept;

/// <summary>
/// Duplicates the HCCallHandle object.  Use HCHttpCallCloseHandle to close it.
/// </summary>
/// <param name="call">The handle of the HTTP call</param>
/// <returns>Returns the duplicated handle.</returns>
STDAPI_(HCCallHandle) HCHttpCallDuplicateHandle(
    _In_ HCCallHandle call
    ) noexcept;

/// <summary>
/// Decrements the reference count on the call object. 
/// When the HCCallHandle ref count is 0, HCHttpCallCloseHandle() will 
/// free the memory associated with the HCCallHandle
/// </summary>
/// <param name="call">The handle of the HTTP call</param>
/// <returns>Result code for this API operation.  Possible values are S_OK, E_INVALIDARG, or E_FAIL.</returns>
STDAPI HCHttpCallCloseHandle(
    _In_ HCCallHandle call
    ) noexcept;

/// <summary>
/// Returns a unique uint64_t which identifies this HTTP call object
/// </summary>
/// <param name="call">The handle of the HTTP call</param>
/// <returns>Returns a unique uint64_t which identifies this HTTP call object or 0 if invalid</returns>
STDAPI_(uint64_t) HCHttpCallGetId(
    _In_ HCCallHandle call
    ) noexcept;

/// <summary>
/// Enables or disables tracing for this specific HTTP call
/// </summary>
/// <param name="call">The handle of the HTTP call</param>
/// <param name="traceCall">Trace this call</param>
/// <returns>Result code for this API operation.  Possible values are S_OK, E_INVALIDARG, or E_FAIL.</returns>
STDAPI HCHttpCallSetTracing(
    _In_ HCCallHandle call,
    _In_ bool traceCall
    ) noexcept;

/// <summary>
/// Gets the request url for the HTTP call
/// </summary>
/// <param name="call">The handle of the HTTP call</param>
/// <param name="url">
/// The UTF-8 encoded url body string of the HTTP call
/// The memory for the returned string pointer remains valid for the life of the HCCallHandle object until HCHttpCallCloseHandle() is called on it.
/// </param>
/// <returns>Result code for this API operation.  Possible values are S_OK, E_INVALIDARG, E_OUTOFMEMORY, or E_FAIL.</returns>
STDAPI HCHttpCallGetRequestUrl(
    _In_ HCCallHandle call,
    _Out_ const char** url
    ) noexcept;

/////////////////////////////////////////////////////////////////////////////////////////
// HttpCallRequest Set APIs
//

/// <summary>
/// Sets the url and method for the HTTP call
/// This must be called prior to calling HCHttpCallPerformAsync.
/// </summary>
/// <param name="call">The handle of the HTTP call</param>
/// <param name="method">UTF-8 encoded method for the HTTP call</param>
/// <param name="url">UTF-8 encoded URL for the HTTP call</param>
/// <returns>Result code for this API operation.  Possible values are S_OK, E_INVALIDARG, E_OUTOFMEMORY, or E_FAIL.</returns>
STDAPI HCHttpCallRequestSetUrl(
    _In_ HCCallHandle call,
    _In_z_ const char* method,
    _In_z_ const char* url
    ) noexcept;

/// <summary>
/// Set the request body bytes of the HTTP call
/// This must be called prior to calling HCHttpCallPerformAsync.
/// </summary> 
/// <param name="call">The handle of the HTTP call</param>
/// <param name="requestBodyBytes">The request body bytes of the HTTP call.</param>
/// <param name="requestBodySize">The length in bytes of the body being set.</param>
/// <returns>Result code for this API operation.  Possible values are S_OK, E_INVALIDARG, E_OUTOFMEMORY, or E_FAIL.</returns>
STDAPI HCHttpCallRequestSetRequestBodyBytes(
    _In_ HCCallHandle call,
    _In_reads_bytes_(requestBodySize) const uint8_t* requestBodyBytes,
    _In_ uint32_t requestBodySize
    ) noexcept;

/// <summary>
/// Set the request body string of the HTTP call
/// This must be called prior to calling HCHttpCallPerformAsync.
/// </summary> 
/// <param name="call">The handle of the HTTP call</param>
/// <param name="requestBodyString">The UTF-8 encoded request body string of the HTTP call.</param>
/// <returns>Result code for this API operation.  Possible values are S_OK, E_INVALIDARG, E_OUTOFMEMORY, or E_FAIL.</returns>
STDAPI HCHttpCallRequestSetRequestBodyString(
    _In_ HCCallHandle call,
    _In_z_ const char* requestBodyString
    ) noexcept;

/// <summary>
/// Set a request header for the HTTP call
/// This must be called prior to calling HCHttpCallPerformAsync.
/// </summary>
/// <param name="call">The handle of the HTTP call</param>
/// <param name="headerName">UTF-8 encoded request header name for the HTTP call</param>
/// <param name="headerValue">UTF-8 encoded request header value for the HTTP call</param>
/// <param name="allowTracing">Set to false to skip tracing this request header, for example if it contains private information</param>
/// <returns>Result code for this API operation.  Possible values are S_OK, E_INVALIDARG, E_OUTOFMEMORY, or E_FAIL.</returns>
STDAPI HCHttpCallRequestSetHeader(
    _In_ HCCallHandle call,
    _In_z_ const char* headerName,
    _In_z_ const char* headerValue,
    _In_ bool allowTracing
    ) noexcept;

/// <summary>
/// Sets if retry is allowed for this HTTP call
/// Defaults to true 
/// This must be called prior to calling HCHttpCallPerformAsync.
/// </summary>
/// <param name="call">The handle of the HTTP call.  Pass nullptr to set the default for future calls</param>
/// <param name="retryAllowed">If retry is allowed for this HTTP call</param>
/// <returns>Result code for this API operation.  Possible values are S_OK, or E_FAIL.</returns>
STDAPI HCHttpCallRequestSetRetryAllowed(
    _In_opt_ HCCallHandle call,
    _In_ bool retryAllowed
    ) noexcept;

/// <summary>
/// ID number of this REST endpoint used to cache the Retry-After header for fast fail.
/// This must be called prior to calling HCHttpCallPerformAsync.
/// </summary>
/// <param name="call">The handle of the HTTP call.  Pass nullptr to set the default for future calls</param>
/// <param name="retryAfterCacheId">ID number of this REST endpoint used to cache the Retry-After header for fast fail.  1-1000 are reserved for XSAPI</param>
/// <returns>Result code for this API operation.  Possible values are S_OK, or E_FAIL.</returns>
STDAPI HCHttpCallRequestSetRetryCacheId(
    _In_opt_ HCCallHandle call,
    _In_ uint32_t retryAfterCacheId
    ) noexcept;

/// <summary>
/// Sets the timeout for this HTTP call.
/// Defaults to 30 seconds
/// This must be called prior to calling HCHttpCallPerformAsync.
/// </summary>
/// <param name="call">The handle of the HTTP call.  Pass nullptr to set the default for future calls</param>
/// <param name="timeoutInSeconds">The timeout for this HTTP call.</param>
/// <returns>Result code for this API operation.  Possible values are S_OK, or E_FAIL.</returns>
STDAPI HCHttpCallRequestSetTimeout(
    _In_opt_ HCCallHandle call,
    _In_ uint32_t timeoutInSeconds
    ) noexcept;

/// <summary>
/// Sets the HTTP retry delay in seconds. The default and minimum delay is 2 seconds.
/// 
/// Retries are delayed using a exponential back off.  By default, it will delay 2 seconds then the 
/// next retry will delay 4 seconds, then 8 seconds, and so on up to a max of 1 min until either
/// the call succeeds or the HTTP timeout window is reached, at which point the call will fail.
/// The delay is also jittered between the current and next delay to spread out service load.
/// The default for the HTTP timeout window is 20 seconds and can be changed using HCSettingsSetTimeoutWindow()
/// 
/// If the service returns an an HTTP error with a "Retry-After" header, then all future calls to that API 
/// will immediately fail with the original error without contacting the service until the "Retry-After" 
/// time has been reached.
///
/// Idempotent service calls are retried when a network error occurs or the server responds with 
/// one of these HTTP status codes:
/// 408 (Request Timeout)
/// 429 (Too Many Requests)
/// 500 (Internal Server Error)
/// 502 (Bad Gateway)
/// 503 (Service Unavailable)
/// 504 (Gateway Timeout)
///
/// This must be called prior to calling HCHttpCallPerformAsync.
/// </summary>
/// <param name="call">The handle of the HTTP call.  Pass nullptr to set the default for future calls</param>
/// <param name="retryDelayInSeconds">The retry delay in seconds</param>
/// <returns>Result code for this API operation.  Possible values are S_OK, or E_FAIL.</returns>
STDAPI HCHttpCallRequestSetRetryDelay(
    _In_opt_ HCCallHandle call,
    _In_ uint32_t retryDelayInSeconds
    ) noexcept;

/// <summary>
/// Sets the HTTP timeout window in seconds.
///
/// This controls how long to spend attempting to retry idempotent service calls before failing.
/// The default is 20 seconds
///
/// Idempotent service calls are retried when a network error occurs or the server responds 
/// with one of these HTTP status codes:
/// 408 (Request Timeout)
/// 429 (Too Many Requests)
/// 500 (Internal Server Error)
/// 502 (Bad Gateway)
/// 503 (Service Unavailable)
/// 504 (Gateway Timeout)
///
/// This must be called prior to calling HCHttpCallPerformAsync.
/// </summary>
/// <param name="call">The handle of the HTTP call.  Pass nullptr to set the default for future calls</param>
/// <param name="timeoutWindowInSeconds">The timeout window in seconds</param>
/// <returns>Result code for this API operation.  Possible values are S_OK, or E_FAIL.</returns>
STDAPI HCHttpCallRequestSetTimeoutWindow(
    _In_opt_ HCCallHandle call,
    _In_ uint32_t timeoutWindowInSeconds
    ) noexcept;


/////////////////////////////////////////////////////////////////////////////////////////
// HttpCallResponse Get APIs
// 

/// <summary>
/// Get the response body string of the HTTP call
/// This can only be called after calling HCHttpCallPerformAsync when the HTTP task is completed.
/// </summary>
/// <param name="call">The handle of the HTTP call</param>
/// <param name="responseString">
/// The UTF-8 encoded response body string of the HTTP call
/// The memory for the returned string pointer remains valid for the life of the HCCallHandle object until HCHttpCallCloseHandle() is called on it.
/// </param>
/// <returns>Result code for this API operation.  Possible values are S_OK, E_INVALIDARG, or E_FAIL.</returns>
STDAPI HCHttpCallResponseGetResponseString(
    _In_ HCCallHandle call,
    _Out_ const char** responseString
    ) noexcept;

/// <summary>
/// Get the response body buffer size of the HTTP call
/// This can only be called after calling HCHttpCallPerformAsync when the HTTP task is completed.
/// </summary>
/// <param name="call">The handle of the HTTP call</param>
/// <param name="bufferSize">The response body buffer size of the HTTP call</param>
/// <returns>Result code for this API operation.  Possible values are S_OK, E_INVALIDARG, or E_FAIL.</returns>
STDAPI HCHttpCallResponseGetResponseBodyBytesSize(
    _In_ HCCallHandle call,
    _Out_ size_t* bufferSize
    ) noexcept;

/// <summary>
/// Get the response body buffer of the HTTP call
/// This can only be called after calling HCHttpCallPerformAsync when the HTTP task is completed.
/// </summary>
/// <param name="call">The handle of the HTTP call</param>
/// <param name="bufferSize">The response body buffer size being passed in</param>
/// <param name="buffer">The buffer to be written to.</param>
/// <param name="bufferUsed">The actual number of bytes written to the buffer.</param>
/// <returns>Result code for this API operation.  Possible values are S_OK, E_INVALIDARG, or E_FAIL.</returns>
STDAPI HCHttpCallResponseGetResponseBodyBytes(
    _In_ HCCallHandle call,
    _In_ size_t bufferSize,
    _Out_writes_bytes_to_opt_(bufferSize, *bufferUsed) uint8_t* buffer,
    _Out_opt_ size_t* bufferUsed
    ) noexcept;

/// <summary>
/// Get the HTTP status code of the HTTP call response
/// This can only be called after calling HCHttpCallPerformAsync when the HTTP task is completed.
/// </summary>
/// <param name="call">The handle of the HTTP call</param>
/// <param name="statusCode">the HTTP status code of the HTTP call response</param>
/// <returns>Result code for this API operation.  Possible values are S_OK, E_INVALIDARG, or E_FAIL.</returns>
STDAPI HCHttpCallResponseGetStatusCode(
    _In_ HCCallHandle call,
    _Out_ uint32_t* statusCode
    ) noexcept;

/// <summary>
/// Get the network error code of the HTTP call
/// This can only be called after calling HCHttpCallPerformAsync when the HTTP task is completed.
/// </summary>
/// <param name="call">The handle of the HTTP call</param>
/// <param name="networkErrorCode">The network error code of the HTTP call. Possible values are S_OK, or E_FAIL.</param>
/// <param name="platformNetworkErrorCode">The platform specific network error code of the HTTP call to be used for tracing / debugging</param>
/// <returns>Result code for this API operation.  Possible values are S_OK, E_INVALIDARG, or E_FAIL.</returns>
STDAPI HCHttpCallResponseGetNetworkErrorCode(
    _In_ HCCallHandle call,
    _Out_ HRESULT* networkErrorCode,
    _Out_ uint32_t* platformNetworkErrorCode
    ) noexcept;

/// <summary>
/// Get the platform network error message of the HTTP call
/// This can only be called after calling HCHttpCallPerformAsync when the HTTP task is completed.
/// </summary>
/// <param name="call">The handle of the HTTP call</param>
/// <param name="platformNetworkErrorMessage">The platform specific network error message of the HTTP call to be used for tracing / debugging</param>
/// <returns>Result code for this API operation. Possible values are S_OK, E_INVALIDARG, or E_FAIL.</returns>
STDAPI HCHttpCallResponseGetPlatformNetworkErrorMessage(
    _In_ HCCallHandle call,
    _Out_ const char** platformNetworkErrorMessage
    ) noexcept;

/// <summary>
/// Get a response header for the HTTP call for a given header name
/// This can only be called after calling HCHttpCallPerformAsync when the HTTP task is completed.
/// </summary>
/// <param name="call">The handle of the HTTP call</param>
/// <param name="headerName">UTF-8 encoded response header name for the HTTP call
/// The memory for the returned string pointer remains valid for the life of the HCCallHandle object until HCHttpCallCloseHandle() is called on it.
/// </param>
/// <param name="headerValue">UTF-8 encoded response header value for the HTTP call.
/// Returns nullptr if the header doesn't exist.
/// The memory for the returned string pointer remains valid for the life of the HCCallHandle object until HCHttpCallCloseHandle() is called on it.
/// </param>
/// <returns>Result code for this API operation.  Possible values are S_OK, E_INVALIDARG, or E_FAIL.</returns>
STDAPI HCHttpCallResponseGetHeader(
    _In_ HCCallHandle call,
    _In_z_ const char* headerName,
    _Out_ const char** headerValue
    ) noexcept;

/// <summary>
/// Gets the number of response headers in the HTTP call
/// This can only be called after calling HCHttpCallPerformAsync when the HTTP task is completed.
/// </summary>
/// <param name="call">The handle of the HTTP call</param>
/// <param name="numHeaders">The number of response headers in the HTTP call</param>
/// <returns>Result code for this API operation.  Possible values are S_OK, E_INVALIDARG, or E_FAIL.</returns>
STDAPI HCHttpCallResponseGetNumHeaders(
    _In_ HCCallHandle call,
    _Out_ uint32_t* numHeaders
    ) noexcept;

/// <summary>
/// Gets the response headers at specific zero based index in the HTTP call.
/// Use HCHttpCallResponseGetNumHeaders() to know how many response headers there are in the HTTP call.
/// This can only be called after calling HCHttpCallPerformAsync when the HTTP task is completed.
/// </summary>
/// <param name="call">The handle of the HTTP call</param>
/// <param name="headerIndex">Specific zero based index of the response header</param>
/// <param name="headerName">UTF-8 encoded response header name for the HTTP call.
/// The memory for the returned string pointer remains valid for the life of the HCCallHandle object until HCHttpCallCloseHandle() is called on it.
/// </param>
/// <param name="headerValue">UTF-8 encoded response header value for the HTTP call.
/// The memory for the returned string pointer remains valid for the life of the HCCallHandle object until HCHttpCallCloseHandle() is called on it.
/// </param>
/// <returns>Result code for this API operation.  Possible values are S_OK, E_INVALIDARG, or E_FAIL.</returns>
STDAPI HCHttpCallResponseGetHeaderAtIndex(
    _In_ HCCallHandle call,
    _In_ uint32_t headerIndex,
    _Out_ const char** headerName,
    _Out_ const char** headerValue
    ) noexcept;

#if !HC_NOWEBSOCKETS
/////////////////////////////////////////////////////////////////////////////////////////
// WebSocket APIs
// 

/// <summary>
/// A callback invoked every time a WebSocket receives an incoming message
/// </summary>
/// <param name="websocket">Handle to the WebSocket that this message was sent to</param>
/// <param name="incomingBodyString">UTF-8 encoded body of the incoming message as a string value, only if the message type is UTF-8.</param>
/// <param name="functionContext">Client context to pass to callback function.</param>
typedef void
(CALLBACK* HCWebSocketMessageFunction)(
    _In_ HCWebsocketHandle websocket,
    _In_z_ const char* incomingBodyString,
    _In_ void* functionContext
    );

/// <summary>
/// A callback invoked every time a WebSocket receives an incoming binary message
/// </summary>
/// <param name="websocket">Handle to the WebSocket that this message was sent to</param>
/// <param name="incomingBodyPayload"></param>
/// <param name="incomingBodyPayloadSize"></param>
/// <param name="functionContext">Client context to pass to callback function.</param>
typedef void
(CALLBACK* HCWebSocketBinaryMessageFunction)(
    _In_ HCWebsocketHandle websocket,
    _In_reads_bytes_(payloadSize) const uint8_t* payloadBytes,
    _In_ uint32_t payloadSize,
    _In_ void* functionContext
    );

/// <summary>
/// A callback invoked when a WebSocket is closed
/// </summary>
/// <param name="websocket">Handle to the WebSocket</param>
/// <param name="closeStatus">The status of why the WebSocket was closed</param>
/// <param name="functionContext">Client context to pass to callback function.</param>
typedef void
(CALLBACK* HCWebSocketCloseEventFunction)(
    _In_ HCWebsocketHandle websocket,
    _In_ HCWebSocketCloseStatus closeStatus,
    _In_ void* functionContext
    );

/// <summary>
/// Creates an WebSocket handle
///
/// WebSocket usage:
/// Create a WebSocket handle using HCWebSocketCreate()
/// Call HCWebSocketSetProxyUri() and HCWebSocketSetHeader() to prepare the HCWebsocketHandle
/// Call HCWebSocketConnectAsync() to connect the WebSocket using the HCWebsocketHandle.
/// Call HCWebSocketSendMessageAsync() to send a message to the WebSocket using the HCWebsocketHandle.
/// Call HCWebSocketDisconnect() to disconnect the WebSocket using the HCWebsocketHandle.
/// Call HCWebSocketCloseHandle() when done with the HCWebsocketHandle to free the associated memory
/// </summary>
/// <param name="websocket">The handle of the websocket</param>
/// <param name="messageFunc">A pointer to the message handling callback to use, or a null pointer to remove.</param>
/// <param name="binaryMessageFunc">A pointer to the binary message handling callback to use, or a null pointer to remove.</param>
/// <param name="closeFunc">A pointer to the close callback to use, or a null pointer to remove.</param>
/// <param name="functionContext">Client context to pass to callback function.</param>
/// <returns>Result code for this API operation.  Possible values are S_OK, E_INVALIDARG, or E_FAIL.</returns>
STDAPI HCWebSocketCreate(
    _Out_ HCWebsocketHandle* websocket,
    _In_opt_ HCWebSocketMessageFunction messageFunc,
    _In_opt_ HCWebSocketBinaryMessageFunction binaryMessageFunc,
    _In_opt_ HCWebSocketCloseEventFunction closeFunc,
    _In_opt_ void* functionContext
    ) noexcept;

/// <summary>
/// Set the proxy URI for the WebSocket
/// This must be called prior to calling HCWebSocketConnectAsync.
/// </summary>
/// <param name="websocket">The handle of the WebSocket</param>
/// <param name="proxyUri">The UTF-8 encoded proxy URI for the WebSocket</param>
/// <returns>Result code for this API operation.  Possible values are S_OK, E_INVALIDARG, E_OUTOFMEMORY, or E_FAIL.</returns>
STDAPI HCWebSocketSetProxyUri(
    _In_ HCWebsocketHandle websocket,
    _In_z_ const char* proxyUri
    ) noexcept;

/// <summary>
/// Set a header for the WebSocket
/// This must be called prior to calling HCWebSocketConnectAsync.
/// </summary>
/// <param name="websocket">The handle of the WebSocket</param>
/// <param name="headerName">UTF-8 encoded header name for the WebSocket</param>
/// <param name="headerValue">UTF-8 encoded header value for the WebSocket</param>
/// <returns>Result code for this API operation.  Possible values are S_OK, E_INVALIDARG, E_OUTOFMEMORY, or E_FAIL.</returns>
STDAPI HCWebSocketSetHeader(
    _In_ HCWebsocketHandle websocket,
    _In_z_ const char* headerName,
    _In_z_ const char* headerValue
    ) noexcept;

/// <summary>
/// Gets the WebSocket functions to allow callers to respond to incoming messages and WebSocket close events.
/// </summary>
/// <param name="websocket">The handle of the websocket</param>
/// <param name="messageFunc">A pointer to the message handling callback to use, or a null pointer to remove.</param>
/// <param name="binaryMessageFunc">A pointer to the binary message handling callback to use, or a null pointer to remove.</param>
/// <param name="closeFunc">A pointer to the close callback to use, or a null pointer to remove.</param>
/// <param name="functionContext">Client context to pass to callback function.</param>
/// <returns>Result code for this API operation.  Possible values are S_OK, E_INVALIDARG, E_HC_NOT_INITIALISED, or E_FAIL.</returns>
STDAPI HCWebSocketGetEventFunctions(
    _In_ HCWebsocketHandle websocket,
    _Out_opt_ HCWebSocketMessageFunction* messageFunc,
    _Out_opt_ HCWebSocketBinaryMessageFunction* binaryMessageFunc,
    _Out_opt_ HCWebSocketCloseEventFunction* closeFunc,
    _Out_ void** functionContext
    ) noexcept;


/// <summary>
/// Used by HCWebSocketConnectAsync() and HCWebSocketSendMessageAsync()
/// </summary>
typedef struct WebSocketCompletionResult
{
    /// <summary>The handle of the HTTP call</summary>
    HCWebsocketHandle websocket;

    /// <summary>The error code of the call. Possible values are S_OK, or E_FAIL.</summary>
    HRESULT errorCode;

    /// <summary>The platform specific network error code of the call to be used for tracing / debugging</summary>
    uint32_t platformErrorCode;
} WebSocketCompletionResult;

/// <summary>
/// Connects to the WebSocket.
/// To get the result, first call HCGetWebSocketConnectResult
/// inside the AsyncBlock callback or after the AsyncBlock is complete.
/// On UWP and XDK, the connection thread is owned and controlled by Windows::Networking::Sockets::MessageWebSocket.
/// On Win32, iOS, and Android, all background work (including initial connection process) will be added to the queue
/// in the provided XAsyncBlock. LibHttpClient will create a reference to that queue but it is the responsibility of the
/// caller to dispatch that queue for as long as the websocket connection is active. Note that work for 
/// HCWebSocketSendMessageAsync calls can be assigned to a separate queue if desired.
/// </summary>
/// <param name="uri">The UTF-8 encoded URI to connect to</param>
/// <param name="subProtocol">The UTF-8 encoded subProtocol to connect to</param>
/// <param name="websocket">The handle of the WebSocket</param>
/// <param name="asyncBlock">The XAsyncBlock that defines the async operation</param>
/// <returns>Result code for this API operation.  Possible values are S_OK, E_INVALIDARG, E_OUTOFMEMORY, or E_FAIL.</returns>
STDAPI HCWebSocketConnectAsync(
    _In_z_ const char* uri,
    _In_z_ const char* subProtocol,
    _In_ HCWebsocketHandle websocket,
    _Inout_ XAsyncBlock* asyncBlock
    ) noexcept;

/// <summary>
/// Gets the result for HCGetWebSocketConnectResult.
/// </summary>
/// <param name="asyncBlock">The XAsyncBlock that defines the async operation</param>
/// <param name="result">Pointer to the result payload</param>
/// <returns>Result code for this API operation.  Possible values are S_OK, E_INVALIDARG, E_OUTOFMEMORY, or E_FAIL.</returns>
STDAPI HCGetWebSocketConnectResult(
    _Inout_ XAsyncBlock* asyncBlock,
    _In_ WebSocketCompletionResult* result
    ) noexcept;

/// <summary>
/// Send message the WebSocket
/// To get the result, first call HCGetWebSocketSendMessageResult
/// inside the AsyncBlock callback or after the AsyncBlock is complete.
/// </summary>
/// <param name="websocket">Handle to the WebSocket</param>
/// <param name="message">The UTF-8 encoded message to send</param>
/// <param name="asyncBlock">The XAsyncBlock that defines the async operation</param>
/// <returns>Result code for this API operation.  Possible values are S_OK, E_INVALIDARG, or E_FAIL.</returns>
STDAPI HCWebSocketSendMessageAsync(
    _In_ HCWebsocketHandle websocket,
    _In_z_ const char* message,
    _Inout_ XAsyncBlock* asyncBlock
    ) noexcept;

/// <summary>
/// Send binary message to the WebSocket
/// To get the result, first call HCGetWebSocketSendMessageResult
/// inside the AsyncBlock callback or after the AsyncBlock is complete.
/// </summary>
/// <param name="websocket">Handle to the WebSocket</param>
/// <param name="payloadBytes"></param>
/// <param name="payloadSize"></param>
/// <param name="asyncBlock">The AsyncBlock that defines the async operation</param>
/// <returns>Result code for this API operation.  Possible values are S_OK, E_INVALIDARG, or E_FAIL.</returns>
STDAPI HCWebSocketSendBinaryMessageAsync(
    _In_ HCWebsocketHandle websocket,
    _In_reads_bytes_(payloadSize) const uint8_t* payloadBytes,
    _In_ uint32_t payloadSize,
    _Inout_ XAsyncBlock* asyncBlock
    ) noexcept;

/// <summary>
/// Gets the result from HCWebSocketSendMessage 
/// </summary>
/// <param name="asyncBlock">The XAsyncBlock that defines the async operation</param>
/// <param name="result">Pointer to the result payload</param>
/// <returns>Returns the duplicated handle.</returns>
STDAPI HCGetWebSocketSendMessageResult(
    _Inout_ XAsyncBlock* asyncBlock,
    _In_ WebSocketCompletionResult* result
    ) noexcept;

/// <summary>
/// Disconnects / closes the WebSocket
/// </summary>
/// <param name="websocket">Handle to the WebSocket</param>
/// <returns>Result code for this API operation.  Possible values are S_OK, E_INVALIDARG, or E_FAIL.</returns>
STDAPI HCWebSocketDisconnect(
    _In_ HCWebsocketHandle websocket
    ) noexcept;

/// <summary>
/// Increments the reference count on the call object.
/// </summary>
/// <param name="websocket">Handle to the WebSocket</param>
/// <returns>Returns the duplicated handle.</returns>
STDAPI_(HCWebsocketHandle) HCWebSocketDuplicateHandle(
    _In_ HCWebsocketHandle websocket
    ) noexcept;

/// <summary>
/// Decrements the reference count on the WebSocket object. 
/// When the ref count is 0, HCWebSocketCloseHandle() will 
/// free the memory associated with the HCWebsocketHandle
/// </summary>
/// <param name="websocket">Handle to the WebSocket</param>
/// <returns>Result code for this API operation.  Possible values are S_OK, E_INVALIDARG, or E_FAIL.</returns>
STDAPI HCWebSocketCloseHandle(
    _In_ HCWebsocketHandle websocket
    ) noexcept;

#endif // !HC_NOWEBSOCKETS

}

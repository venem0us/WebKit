/*
 * Copyright (C) 2026 Glass. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

// Glass: Web Authentication (passkeys, security keys, phones) on Windows, through Windows' WebAuthn API
// (webauthn.dll, Windows 10 1903 and later), the way Chrome, Edge and Firefox do it on Windows. Windows shows its
// own dialogs (Windows Hello, a phone's QR code, a security key's PIN) and does the CTAP work; the browser builds
// the client data, checks the caller, and turns the result into the page's response.

#include "config.h"
#include "WebAuthenticatorCoordinatorProxy.h"

#if ENABLE(WEB_AUTHN) && PLATFORM(WIN)

#include "Logging.h"
#include "RelatedOriginsValidator.h"
#include "WebAuthenticationRequestData.h"
#include "WebPageProxy.h"
#include "WebsiteDataStore.h"
#include <WebCore/AuthenticatorResponseData.h>
#include <WebCore/ExceptionData.h>
#include <WebCore/SecurityOrigin.h>
#include <WebCore/SecurityOriginData.h>
#include <WebCore/WebAuthenticationUtils.h>
#include <windows.h>
#include <webauthn.h>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <wtf/MainThread.h>
#include <wtf/Threading.h>

namespace WebKit {
using namespace WebCore;

namespace {

// webauthn.dll, loaded on first use (it isn't on Windows before 1903, and most pages never need it).
struct WindowsWebAuthn {
    decltype(&::WebAuthNGetApiVersionNumber) getApiVersionNumber { nullptr };
    decltype(&::WebAuthNIsUserVerifyingPlatformAuthenticatorAvailable) isUserVerifyingPlatformAuthenticatorAvailable { nullptr };
    decltype(&::WebAuthNAuthenticatorMakeCredential) makeCredential { nullptr };
    decltype(&::WebAuthNAuthenticatorGetAssertion) getAssertion { nullptr };
    decltype(&::WebAuthNFreeCredentialAttestation) freeCredentialAttestation { nullptr };
    decltype(&::WebAuthNFreeAssertion) freeAssertion { nullptr };
    decltype(&::WebAuthNGetCancellationId) getCancellationId { nullptr };
    decltype(&::WebAuthNCancelCurrentOperation) cancelCurrentOperation { nullptr };
    decltype(&::WebAuthNGetErrorName) getErrorName { nullptr };
    DWORD apiVersion { 0 };

    bool isAvailable() const { return makeCredential && getAssertion && freeCredentialAttestation && freeAssertion; }
};

const WindowsWebAuthn& windowsWebAuthn()
{
    static const WindowsWebAuthn api = [] {
        WindowsWebAuthn api;
        HMODULE module = LoadLibraryExW(L"webauthn.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module)
            return api;
        auto load = [module]<typename T>(T& function, const char* name) {
            function = reinterpret_cast<T>(GetProcAddress(module, name));
        };
        load(api.getApiVersionNumber, "WebAuthNGetApiVersionNumber");
        load(api.isUserVerifyingPlatformAuthenticatorAvailable, "WebAuthNIsUserVerifyingPlatformAuthenticatorAvailable");
        load(api.makeCredential, "WebAuthNAuthenticatorMakeCredential");
        load(api.getAssertion, "WebAuthNAuthenticatorGetAssertion");
        load(api.freeCredentialAttestation, "WebAuthNFreeCredentialAttestation");
        load(api.freeAssertion, "WebAuthNFreeAssertion");
        load(api.getCancellationId, "WebAuthNGetCancellationId");
        load(api.cancelCurrentOperation, "WebAuthNCancelCurrentOperation");
        load(api.getErrorName, "WebAuthNGetErrorName");
        api.apiVersion = api.getApiVersionNumber ? api.getApiVersionNumber() : WEBAUTHN_API_VERSION_1;
        return api;
    }();
    return api;
}

// GLASS_WEBAUTHN_LOG=<file>: a line per step (request, Windows' result, cancellation), for diagnosing on test PCs.
// Only what the request is (create/get, relying party, API version, results); never credentials or user names.
void diagnostic(const char* format, ...)
{
    static const std::optional<std::string> path = []() -> std::optional<std::string> {
        char buffer[MAX_PATH];
        DWORD length = GetEnvironmentVariableA("GLASS_WEBAUTHN_LOG", buffer, MAX_PATH);
        if (!length || length >= MAX_PATH)
            return std::nullopt;
        return std::string(buffer, length);
    }();
    if (!path)
        return;
    char line[512];
    va_list arguments;
    va_start(arguments, format);
    int written = vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    if (written <= 0)
        return;
    SYSTEMTIME time;
    GetLocalTime(&time);
    if (FILE* file = fopen(path->c_str(), "a")) {
        fprintf(file, "%02u:%02u:%02u.%03u [%lu] %s\n", time.wHour, time.wMinute, time.wSecond, time.wMilliseconds, GetCurrentThreadId(), line);
        fclose(file);
    }
}

std::wstring toWide(const String& string)
{
    auto characters = string.wideCharacters();
    return std::wstring(characters.begin(), characters.end());
}

Vector<uint8_t> toBytes(std::span<const uint8_t> span)
{
    return Vector<uint8_t>(span);
}

Vector<uint8_t> toBytes(const BYTE* data, DWORD size)
{
    if (!data || !size)
        return { };
    return Vector<uint8_t>(std::span<const uint8_t> { data, size });
}

RefPtr<ArrayBuffer> toArrayBuffer(const Vector<uint8_t>& bytes)
{
    return ArrayBuffer::create(bytes.span());
}

DWORD transportBits(const Vector<AuthenticatorTransport>& transports, DWORD apiVersion)
{
    DWORD bits = 0;
    for (auto transport : transports) {
        switch (transport) {
        case AuthenticatorTransport::Usb: bits |= WEBAUTHN_CTAP_TRANSPORT_USB; break;
        case AuthenticatorTransport::Nfc: bits |= WEBAUTHN_CTAP_TRANSPORT_NFC; break;
        case AuthenticatorTransport::Ble: bits |= WEBAUTHN_CTAP_TRANSPORT_BLE; break;
        case AuthenticatorTransport::Internal: bits |= WEBAUTHN_CTAP_TRANSPORT_INTERNAL; break;
        // Older Windows rejects transport bits it doesn't know, which would turn a hint into a failure.
        case AuthenticatorTransport::Cable:
        case AuthenticatorTransport::Hybrid: if (apiVersion >= WEBAUTHN_API_VERSION_6) bits |= WEBAUTHN_CTAP_TRANSPORT_HYBRID; break;
        case AuthenticatorTransport::SmartCard: if (apiVersion >= WEBAUTHN_API_VERSION_6) bits |= WEBAUTHN_CTAP_TRANSPORT_SMART_CARD; break;
        }
    }
    return bits;
}

Vector<AuthenticatorTransport> transportsFromBits(DWORD bits)
{
    Vector<AuthenticatorTransport> transports;
    if (bits & WEBAUTHN_CTAP_TRANSPORT_USB) transports.append(AuthenticatorTransport::Usb);
    if (bits & WEBAUTHN_CTAP_TRANSPORT_NFC) transports.append(AuthenticatorTransport::Nfc);
    if (bits & WEBAUTHN_CTAP_TRANSPORT_BLE) transports.append(AuthenticatorTransport::Ble);
    if (bits & WEBAUTHN_CTAP_TRANSPORT_INTERNAL) transports.append(AuthenticatorTransport::Internal);
    if (bits & WEBAUTHN_CTAP_TRANSPORT_HYBRID) transports.append(AuthenticatorTransport::Hybrid);
    if (bits & WEBAUTHN_CTAP_TRANSPORT_SMART_CARD) transports.append(AuthenticatorTransport::SmartCard);
    return transports;
}

DWORD userVerification(UserVerificationRequirement requirement)
{
    switch (requirement) {
    case UserVerificationRequirement::Required: return WEBAUTHN_USER_VERIFICATION_REQUIREMENT_REQUIRED;
    case UserVerificationRequirement::Preferred: return WEBAUTHN_USER_VERIFICATION_REQUIREMENT_PREFERRED;
    case UserVerificationRequirement::Discouraged: return WEBAUTHN_USER_VERIFICATION_REQUIREMENT_DISCOURAGED;
    }
    return WEBAUTHN_USER_VERIFICATION_REQUIREMENT_ANY;
}

DWORD attachment(std::optional<AuthenticatorAttachment> attachment)
{
    if (!attachment)
        return WEBAUTHN_AUTHENTICATOR_ATTACHMENT_ANY;
    return *attachment == AuthenticatorAttachment::Platform ? WEBAUTHN_AUTHENTICATOR_ATTACHMENT_PLATFORM : WEBAUTHN_AUTHENTICATOR_ATTACHMENT_CROSS_PLATFORM;
}

// Everything the worker thread needs, copied out of the engine's objects (which stay on the main thread).
struct WindowsRequest {
    bool isCreate { false };
    HWND window { nullptr };
    DWORD apiVersion { 0 };
    Vector<uint8_t> clientDataJSON;
    std::wstring relyingPartyID;
    std::wstring relyingPartyName;
    Vector<uint8_t> userID;
    std::wstring userName;
    std::wstring userDisplayName;
    Vector<LONG> algorithms;
    Vector<std::pair<Vector<uint8_t>, DWORD>> credentials; // excluded (create) or allowed (get), with transports
    DWORD timeoutMilliseconds { 300000 };
    DWORD attachment { WEBAUTHN_AUTHENTICATOR_ATTACHMENT_ANY };
    DWORD userVerification { WEBAUTHN_USER_VERIFICATION_REQUIREMENT_PREFERRED };
    DWORD attestation { WEBAUTHN_ATTESTATION_CONVEYANCE_PREFERENCE_NONE };
    bool requireResidentKey { false };
    bool preferResidentKey { false };
    bool privateMode { false };
    std::optional<GUID> cancellationID;
};

struct WindowsResult {
    HRESULT hr { E_FAIL };
    String errorName;
    Vector<uint8_t> rawID;
    Vector<uint8_t> attestationObject;
    Vector<uint8_t> authenticatorData;
    Vector<uint8_t> signature;
    Vector<uint8_t> userHandle;
    DWORD usedTransport { 0 };
    DWORD transports { 0 };
    std::optional<bool> residentKey;
};

// Runs on the worker thread: Windows' dialogs are up until this returns.
WindowsResult runWindowsRequest(WindowsRequest& request)
{
    auto& api = windowsWebAuthn();
    WindowsResult result;
    GUID* cancellationID = request.cancellationID ? &*request.cancellationID : nullptr;

    WEBAUTHN_CLIENT_DATA clientData { WEBAUTHN_CLIENT_DATA_CURRENT_VERSION, static_cast<DWORD>(request.clientDataJSON.size()), request.clientDataJSON.mutableSpan().data(), WEBAUTHN_HASH_ALGORITHM_SHA_256 };

    Vector<WEBAUTHN_CREDENTIAL_EX> credentials;
    for (auto& [id, transports] : request.credentials)
        credentials.append({ WEBAUTHN_CREDENTIAL_EX_CURRENT_VERSION, static_cast<DWORD>(id.size()), id.mutableSpan().data(), WEBAUTHN_CREDENTIAL_TYPE_PUBLIC_KEY, transports });
    Vector<PWEBAUTHN_CREDENTIAL_EX> credentialPointers;
    for (auto& credential : credentials)
        credentialPointers.append(&credential);
    WEBAUTHN_CREDENTIAL_LIST credentialList { static_cast<DWORD>(credentialPointers.size()), credentialPointers.mutableSpan().data() };

    if (request.isCreate) {
        WEBAUTHN_RP_ENTITY_INFORMATION relyingParty { WEBAUTHN_RP_ENTITY_INFORMATION_CURRENT_VERSION, request.relyingPartyID.c_str(), request.relyingPartyName.c_str(), nullptr };
        WEBAUTHN_USER_ENTITY_INFORMATION user { WEBAUTHN_USER_ENTITY_INFORMATION_CURRENT_VERSION, static_cast<DWORD>(request.userID.size()), request.userID.mutableSpan().data(), request.userName.c_str(), nullptr, request.userDisplayName.c_str() };
        Vector<WEBAUTHN_COSE_CREDENTIAL_PARAMETER> parameters;
        for (auto algorithm : request.algorithms)
            parameters.append({ WEBAUTHN_COSE_CREDENTIAL_PARAMETER_CURRENT_VERSION, WEBAUTHN_CREDENTIAL_TYPE_PUBLIC_KEY, algorithm });
        WEBAUTHN_COSE_CREDENTIAL_PARAMETERS parameterList { static_cast<DWORD>(parameters.size()), parameters.mutableSpan().data() };

        // The newest options structure this Windows understands (it rejects a version it doesn't know).
        WEBAUTHN_AUTHENTICATOR_MAKE_CREDENTIAL_OPTIONS options { };
        options.dwVersion = request.apiVersion >= WEBAUTHN_API_VERSION_4 ? WEBAUTHN_AUTHENTICATOR_MAKE_CREDENTIAL_OPTIONS_VERSION_5
            : request.apiVersion >= WEBAUTHN_API_VERSION_3 ? WEBAUTHN_AUTHENTICATOR_MAKE_CREDENTIAL_OPTIONS_VERSION_4 : WEBAUTHN_AUTHENTICATOR_MAKE_CREDENTIAL_OPTIONS_VERSION_3;
        options.dwTimeoutMilliseconds = request.timeoutMilliseconds;
        options.dwAuthenticatorAttachment = request.attachment;
        options.bRequireResidentKey = request.requireResidentKey;
        options.dwUserVerificationRequirement = request.userVerification;
        options.dwAttestationConveyancePreference = request.attestation;
        options.pCancellationId = cancellationID;
        options.pExcludeCredentialList = credentialPointers.isEmpty() ? nullptr : &credentialList;
        if (options.dwVersion >= WEBAUTHN_AUTHENTICATOR_MAKE_CREDENTIAL_OPTIONS_VERSION_4)
            options.bPreferResidentKey = request.preferResidentKey && !request.requireResidentKey;
        else if (request.preferResidentKey)
            options.bRequireResidentKey = TRUE; // no "preferred" before API 3: a passkey is what the site asked for first
        if (options.dwVersion >= WEBAUTHN_AUTHENTICATOR_MAKE_CREDENTIAL_OPTIONS_VERSION_5)
            options.bBrowserInPrivateMode = request.privateMode;

        PWEBAUTHN_CREDENTIAL_ATTESTATION attestation = nullptr;
        diagnostic("makeCredential: api %lu, options v%lu, window %p, %zu excluded, cancellation id %s", request.apiVersion, options.dwVersion, request.window, credentials.size(), cancellationID ? "yes" : "no");
        result.hr = api.makeCredential(request.window, &relyingParty, &user, &parameterList, &clientData, &options, &attestation);
        diagnostic("makeCredential returned 0x%08lx", static_cast<unsigned long>(result.hr));
        if (SUCCEEDED(result.hr) && attestation) {
            result.rawID = toBytes(attestation->pbCredentialId, attestation->cbCredentialId);
            result.attestationObject = toBytes(attestation->pbAttestationObject, attestation->cbAttestationObject);
            result.authenticatorData = toBytes(attestation->pbAuthenticatorData, attestation->cbAuthenticatorData);
            if (attestation->dwVersion >= WEBAUTHN_CREDENTIAL_ATTESTATION_VERSION_3)
                result.usedTransport = attestation->dwUsedTransport;
            if (attestation->dwVersion >= WEBAUTHN_CREDENTIAL_ATTESTATION_VERSION_4)
                result.residentKey = !!attestation->bResidentKey;
            result.transports = attestation->dwVersion >= WEBAUTHN_CREDENTIAL_ATTESTATION_VERSION_8 && attestation->dwTransports ? attestation->dwTransports : result.usedTransport;
        }
        if (attestation)
            api.freeCredentialAttestation(attestation);
    } else {
        WEBAUTHN_AUTHENTICATOR_GET_ASSERTION_OPTIONS options { };
        options.dwVersion = request.apiVersion >= WEBAUTHN_API_VERSION_4 ? WEBAUTHN_AUTHENTICATOR_GET_ASSERTION_OPTIONS_VERSION_6
            : request.apiVersion >= WEBAUTHN_API_VERSION_3 ? WEBAUTHN_AUTHENTICATOR_GET_ASSERTION_OPTIONS_VERSION_5 : WEBAUTHN_AUTHENTICATOR_GET_ASSERTION_OPTIONS_VERSION_4;
        options.dwTimeoutMilliseconds = request.timeoutMilliseconds;
        options.dwAuthenticatorAttachment = request.attachment;
        options.dwUserVerificationRequirement = request.userVerification;
        options.pCancellationId = cancellationID;
        options.pAllowCredentialList = credentialPointers.isEmpty() ? nullptr : &credentialList;
        if (options.dwVersion >= WEBAUTHN_AUTHENTICATOR_GET_ASSERTION_OPTIONS_VERSION_6)
            options.bBrowserInPrivateMode = request.privateMode;

        PWEBAUTHN_ASSERTION assertion = nullptr;
        diagnostic("getAssertion: api %lu, options v%lu, window %p, %zu allowed, cancellation id %s", request.apiVersion, options.dwVersion, request.window, credentials.size(), cancellationID ? "yes" : "no");
        result.hr = api.getAssertion(request.window, request.relyingPartyID.c_str(), &clientData, &options, &assertion);
        diagnostic("getAssertion returned 0x%08lx", static_cast<unsigned long>(result.hr));
        if (SUCCEEDED(result.hr) && assertion) {
            result.rawID = toBytes(assertion->Credential.pbId, assertion->Credential.cbId);
            result.authenticatorData = toBytes(assertion->pbAuthenticatorData, assertion->cbAuthenticatorData);
            result.signature = toBytes(assertion->pbSignature, assertion->cbSignature);
            result.userHandle = toBytes(assertion->pbUserId, assertion->cbUserId);
            if (assertion->dwVersion >= WEBAUTHN_ASSERTION_VERSION_4)
                result.usedTransport = assertion->dwUsedTransport;
        }
        if (assertion)
            api.freeAssertion(assertion);
    }

    if (FAILED(result.hr) && api.getErrorName) {
        if (auto name = api.getErrorName(result.hr))
            result.errorName = String(std::span { reinterpret_cast<const char16_t*>(name), wcslen(name) });
    }
    return result;
}

// The page's error for a failed request, as Chrome and Edge report Windows' errors.
ExceptionData exceptionForWindowsError(HRESULT hr, const String& errorName)
{
    switch (hr) {
    case NTE_EXISTS:
        return { ExceptionCode::InvalidStateError, "The authenticator already holds a credential for this account."_s };
    case NTE_NOT_SUPPORTED:
        return { ExceptionCode::NotSupportedError, "The request isn't supported by Windows or the authenticator."_s };
    case NTE_INVALID_PARAMETER:
        return { ExceptionCode::NotSupportedError, "The request's options aren't valid for Windows' WebAuthn API."_s };
    case NTE_TOKEN_KEYSET_STORAGE_FULL:
        return { ExceptionCode::ConstraintError, "The authenticator has no room for another credential."_s };
    default:
        RELEASE_LOG_ERROR(WebAuthn, "Windows WebAuthn request failed: 0x%08lx %s", static_cast<unsigned long>(hr), errorName.utf8().data());
        return { ExceptionCode::NotAllowedError, "The operation either timed out or was not allowed."_s };
    }
}

} // namespace

void WebAuthenticatorCoordinatorProxy::performRequest(WebAuthenticationRequestData&& data, RequestCompletionHandler&& handler)
{
    auto& api = windowsWebAuthn();
    if (!api.isAvailable())
        return handler({ }, (AuthenticatorAttachment)0, ExceptionData { ExceptionCode::NotSupportedError, "Web Authentication needs Windows 10 version 1903 or later."_s });

    RefPtr webPageProxy = m_webPageProxy.get();
    if (!webPageProxy || !data.frameInfo)
        return handler({ }, (AuthenticatorAttachment)0, ExceptionData { ExceptionCode::InvalidStateError });

    // The UI process checks the caller itself rather than trusting the web process: a secure origin, and a relying
    // party that is its own site (or a related origin the relying party lists in /.well-known/webauthn).
    const auto& callerOrigin = data.frameInfo->securityOrigin;
    Ref origin = callerOrigin.securityOrigin();
    if (!origin->isPotentiallyTrustworthy())
        return handler({ }, (AuthenticatorAttachment)0, ExceptionData { ExceptionCode::SecurityError, "Web Authentication needs a secure (https) page."_s });

    String relyingPartyIdentifier = WTF::switchOn(data.options, [](const PublicKeyCredentialCreationOptions& options) {
        return options.rp.id;
    }, [](const PublicKeyCredentialRequestOptions& options) {
        return options.rpId;
    });
    if (relyingPartyIdentifier.isEmpty())
        relyingPartyIdentifier = callerOrigin.host();

    if (origin->isMatchingRegistrableDomainSuffix(relyingPartyIdentifier))
        return performRequestOnWindows(WTF::move(data), WTF::move(relyingPartyIdentifier), WTF::move(handler));

    RelatedOriginsValidation::validate(*webPageProxy, callerOrigin, relyingPartyIdentifier, [weakThis = WeakPtr { *this }, data = WTF::move(data), relyingPartyIdentifier, handler = WTF::move(handler)](RelatedOriginsValidation::Result&& result) mutable {
        RefPtr protectedThis = weakThis.get();
        if (!protectedThis)
            return handler({ }, (AuthenticatorAttachment)0, ExceptionData { ExceptionCode::InvalidStateError });
        if (!result.isRelated)
            return handler({ }, (AuthenticatorAttachment)0, ExceptionData { ExceptionCode::SecurityError, "The relying party ID is not a registrable domain suffix of, nor equal to the current domain."_s });
        protectedThis->performRequestOnWindows(WTF::move(data), WTF::move(relyingPartyIdentifier), WTF::move(handler));
    });
}

void WebAuthenticatorCoordinatorProxy::performRequestOnWindows(WebAuthenticationRequestData&& data, String&& relyingPartyIdentifier, RequestCompletionHandler&& handler)
{
    auto& api = windowsWebAuthn();
    RefPtr webPageProxy = m_webPageProxy.get();
    if (!webPageProxy)
        return handler({ }, (AuthenticatorAttachment)0, ExceptionData { ExceptionCode::InvalidStateError });

    // Windows runs one ceremony at a time; a page asking again while its dialog is up gets the usual answer.
    if (m_windowsCompletionHandler)
        return handler({ }, (AuthenticatorAttachment)0, ExceptionData { ExceptionCode::NotAllowedError, "A request is already pending."_s });

    WindowsRequest request;
    request.apiVersion = api.apiVersion;
    // Windows' dialogs are owned by the browser window, so they open over it and keep it from being used meanwhile.
    request.window = GetAncestor(reinterpret_cast<HWND>(webPageProxy->viewWidget()), GA_ROOT);
    request.relyingPartyID = toWide(relyingPartyIdentifier);
    request.privateMode = !webPageProxy->websiteDataStore().isPersistent();

    WebAuthn::Scope scope = data.parentOrigin ? WebAuthn::Scope::CrossOrigin : WebAuthn::Scope::SameOrigin;
    auto topOrigin = data.parentOrigin ? data.parentOrigin->toString() : nullString();
    Ref callerOrigin = data.frameInfo->securityOrigin.securityOrigin();
    RefPtr<ArrayBuffer> clientDataJSON;
    std::optional<bool> wantsCredentialProperties;
    std::optional<UserVerificationRequirement> userVerificationRequirement;

    WTF::switchOn(data.options, [&](const PublicKeyCredentialCreationOptions& options) {
        clientDataJSON = buildClientDataJson(ClientDataType::Create, options.challenge, callerOrigin.get(), scope, topOrigin);
        request.isCreate = true;
        request.relyingPartyName = toWide(options.rp.name.isEmpty() ? relyingPartyIdentifier : options.rp.name);
        request.userID = toBytes(options.user.id.span());
        request.userName = toWide(options.user.name);
        request.userDisplayName = toWide(options.user.displayName);
        for (auto& parameter : options.pubKeyCredParams)
            request.algorithms.append(static_cast<LONG>(parameter.alg));
        for (auto& descriptor : options.excludeCredentials)
            request.credentials.append({ toBytes(descriptor.id.span()), transportBits(descriptor.transports, api.apiVersion) });
        if (options.timeout)
            request.timeoutMilliseconds = *options.timeout;
        if (auto& selection = options.authenticatorSelection) {
            request.attachment = attachment(selection->authenticatorAttachment);
            userVerificationRequirement = selection->userVerification;
            auto residentKey = selection->residentKey.value_or(selection->requireResidentKey ? ResidentKeyRequirement::Required : ResidentKeyRequirement::Discouraged);
            request.requireResidentKey = residentKey == ResidentKeyRequirement::Required;
            request.preferResidentKey = residentKey == ResidentKeyRequirement::Preferred;
        }
        switch (options.attestation) {
        case AttestationConveyancePreference::Indirect: request.attestation = WEBAUTHN_ATTESTATION_CONVEYANCE_PREFERENCE_INDIRECT; break;
        case AttestationConveyancePreference::Direct: request.attestation = WEBAUTHN_ATTESTATION_CONVEYANCE_PREFERENCE_DIRECT; break;
        // Enterprise attestation needs the relying party on a managed allowlist; without one it is "none" (spec).
        default: request.attestation = WEBAUTHN_ATTESTATION_CONVEYANCE_PREFERENCE_NONE; break;
        }
        if (options.extensions && options.extensions->credProps && *options.extensions->credProps)
            wantsCredentialProperties = request.requireResidentKey;
    }, [&](const PublicKeyCredentialRequestOptions& options) {
        clientDataJSON = buildClientDataJson(ClientDataType::Get, options.challenge, callerOrigin.get(), scope, topOrigin);
        for (auto& descriptor : options.allowCredentials)
            request.credentials.append({ toBytes(descriptor.id.span()), transportBits(descriptor.transports, api.apiVersion) });
        if (options.timeout)
            request.timeoutMilliseconds = *options.timeout;
        request.attachment = attachment(options.authenticatorAttachment);
        userVerificationRequirement = options.userVerification;
    });
    request.userVerification = userVerification(userVerificationRequirement.value_or(UserVerificationRequirement::Preferred));
    request.timeoutMilliseconds = std::clamp<DWORD>(request.timeoutMilliseconds, 30000, 600000);
    request.clientDataJSON = toBytes(clientDataJSON->span());

    GUID cancellationID { };
    HRESULT cancellationHR = api.getCancellationId ? api.getCancellationId(&cancellationID) : E_NOTIMPL;
    diagnostic("request %s for a page (private %d); WebAuthNGetCancellationId 0x%08lx", request.isCreate ? "create" : "get", request.privateMode, static_cast<unsigned long>(cancellationHR));
    if (SUCCEEDED(cancellationHR)) {
        request.cancellationID = cancellationID;
        memcpySpan(std::span { m_windowsCancellationID }, asByteSpan(cancellationID));
        m_windowsHasCancellationID = true;
    } else
        m_windowsHasCancellationID = false;

    auto requestID = ++m_windowsRequestID;
    m_windowsCompletionHandler = WTF::move(handler);
    bool isCreate = request.isCreate;

    Thread::create("WebAuthn"_s, [request = WTF::move(request), weakThis = WeakPtr { *this }, requestID, isCreate, clientDataJSON = WTF::move(clientDataJSON), wantsCredentialProperties]() mutable {
        auto result = runWindowsRequest(request);
        callOnMainRunLoop([weakThis = WTF::move(weakThis), requestID, isCreate, result = WTF::move(result), clientDataJSON = WTF::move(clientDataJSON), wantsCredentialProperties]() mutable {
            RefPtr protectedThis = weakThis.get();
            if (!protectedThis)
                return;
            if (FAILED(result.hr))
                return protectedThis->finishWindowsRequest(requestID, { }, (AuthenticatorAttachment)0, exceptionForWindowsError(result.hr, result.errorName));

            AuthenticatorResponseData response;
            response.isAuthenticatorAttestationResponse = isCreate;
            response.rawId = toArrayBuffer(result.rawID);
            response.clientDataJSON = WTF::move(clientDataJSON);
            if (isCreate) {
                response.attestationObject = toArrayBuffer(result.attestationObject);
                response.transports = transportsFromBits(result.transports);
                if (wantsCredentialProperties) {
                    AuthenticationExtensionsClientOutputs outputs;
                    outputs.credProps = CredentialPropertiesOutput { result.residentKey.value_or(*wantsCredentialProperties) };
                    response.extensionOutputs = WTF::move(outputs);
                }
            } else {
                response.authenticatorData = toArrayBuffer(result.authenticatorData);
                response.signature = toArrayBuffer(result.signature);
                if (!result.userHandle.isEmpty())
                    response.userHandle = toArrayBuffer(result.userHandle);
            }
            auto attachment = result.usedTransport & WEBAUTHN_CTAP_TRANSPORT_INTERNAL ? AuthenticatorAttachment::Platform : AuthenticatorAttachment::CrossPlatform;
            protectedThis->finishWindowsRequest(requestID, response, attachment, { });
        });
    })->detach();
}

void WebAuthenticatorCoordinatorProxy::finishWindowsRequest(uint64_t requestID, const AuthenticatorResponseData& response, AuthenticatorAttachment attachment, const ExceptionData& exception)
{
    if (requestID != m_windowsRequestID || !m_windowsCompletionHandler)
        return;
    m_windowsHasCancellationID = false;
    auto handler = std::exchange(m_windowsCompletionHandler, nullptr);
    handler(response, attachment, exception);
}

void WebAuthenticatorCoordinatorProxy::cancel(CompletionHandler<void()>&& handler)
{
    // The page aborted (AbortController), navigated or closed. Windows is asked to cancel, but once its "Save your
    // passkey" or sign-in dialog shows it stays until the user closes it (WebAuthNCancelCurrentOperation succeeds and
    // the blocked call keeps waiting; checked outside WebKit on Windows 11, API version 9). So the page gets its
    // answer now, as the spec expects for an abort, and whatever Windows returns later is dropped (finishWindowsRequest
    // ignores a request that was already answered).
    diagnostic("cancel: pending %d, cancellation id %d", !!m_windowsCompletionHandler, m_windowsHasCancellationID);
    if (m_windowsCompletionHandler && m_windowsHasCancellationID) {
        auto& api = windowsWebAuthn();
        if (api.cancelCurrentOperation) {
            GUID cancellationID;
            memcpySpan(asMutableByteSpan(cancellationID), std::span { m_windowsCancellationID });
            HRESULT hr = api.cancelCurrentOperation(&cancellationID);
            diagnostic("WebAuthNCancelCurrentOperation returned 0x%08lx", static_cast<unsigned long>(hr));
        }
    }
    if (m_windowsCompletionHandler)
        finishWindowsRequest(m_windowsRequestID, { }, (AuthenticatorAttachment)0, ExceptionData { ExceptionCode::NotAllowedError, "The operation was aborted."_s });
    handler();
}

void WebAuthenticatorCoordinatorProxy::isUserVerifyingPlatformAuthenticatorAvailable(IPC::Connection&, IPC::Untrusted<SecurityOriginData>&&, QueryCompletionHandler&& handler)
{
    // Windows Hello set up (a PIN, face or fingerprint): sites then offer "Sign in with a passkey".
    auto& api = windowsWebAuthn();
    BOOL available = FALSE;
    if (api.isUserVerifyingPlatformAuthenticatorAvailable && FAILED(api.isUserVerifyingPlatformAuthenticatorAvailable(&available)))
        available = FALSE;
    handler(!!available);
}

void WebAuthenticatorCoordinatorProxy::isConditionalMediationAvailable(IPC::Connection&, IPC::Untrusted<SecurityOriginData>&&, QueryCompletionHandler&& handler)
{
    // Passkeys offered in a sign-in field's autofill: Windows has no API for that yet.
    handler(false);
}

void WebAuthenticatorCoordinatorProxy::getClientCapabilities(IPC::Connection&, IPC::Untrusted<SecurityOriginData>&&, CapabilitiesCompletionHandler&& handler)
{
    auto& api = windowsWebAuthn();
    BOOL platform = FALSE;
    if (!api.isUserVerifyingPlatformAuthenticatorAvailable || FAILED(api.isUserVerifyingPlatformAuthenticatorAvailable(&platform)))
        platform = FALSE;
    Vector<KeyValuePair<String, bool>> capabilities {
        { "conditionalCreate"_s, false },
        { "conditionalGet"_s, false },
        { "extension:credProps"_s, api.isAvailable() },
        { "hybridTransport"_s, api.isAvailable() && api.apiVersion >= WEBAUTHN_API_VERSION_6 },
        { "passkeyPlatformAuthenticator"_s, !!platform },
        { "relatedOrigins"_s, true },
        { "signalAllAcceptedCredentials"_s, false },
        { "signalCurrentUserDetails"_s, false },
        { "signalUnknownCredential"_s, false },
        { "userVerifyingPlatformAuthenticator"_s, !!platform },
    };
    handler(WTF::move(capabilities));
}

// Windows has no API for a site to tell the authenticator about removed or renamed credentials: accepted, no effect
// (as the capabilities above say), so sites that send them anyway don't see an error.
void WebAuthenticatorCoordinatorProxy::signalUnknownCredential(IPC::Connection&, IPC::Untrusted<SecurityOriginData>&&, UnknownCredentialOptions&&, CompletionHandler<void(std::optional<ExceptionData>)>&& handler)
{
    handler(std::nullopt);
}

void WebAuthenticatorCoordinatorProxy::signalAllAcceptedCredentials(IPC::Connection&, IPC::Untrusted<SecurityOriginData>&&, AllAcceptedCredentialsOptions&&, CompletionHandler<void(std::optional<ExceptionData>)>&& handler)
{
    handler(std::nullopt);
}

void WebAuthenticatorCoordinatorProxy::signalCurrentUserDetails(IPC::Connection&, IPC::Untrusted<SecurityOriginData>&&, CurrentUserDetailsOptions&&, CompletionHandler<void(std::optional<ExceptionData>)>&& handler)
{
    handler(std::nullopt);
}

} // namespace WebKit

#endif // ENABLE(WEB_AUTHN) && PLATFORM(WIN)

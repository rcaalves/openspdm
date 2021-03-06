/** @file
  SPDM common library.
  It follows the SPDM Specification.

  Copyright (c) 2020, Intel Corporation. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "SpdmResponderLibInternal.h"

/**
  This function calculate the measurement summary hash.

  @param  SpdmContext                  A pointer to the SPDM context.
  @param  MeasurementSummaryHashType   The type of the measurement summary hash.
  @param  MeasurementSummaryHash       The buffer to store the measurement summary hash.

  @retval TRUE  measurement summary hash is generated.
  @retval FALSE measurement summary hash is not generated.
**/
BOOLEAN
SpdmResponderCalculateMeasurementSummaryHash (
  IN  SPDM_DEVICE_CONTEXT  *SpdmContext,
  IN  UINT8                MeasurementSummaryHashType,
  OUT UINT8                *MeasurementSummaryHash
  );

/**
  This function generates the PSK exchange HMAC based upon TH.

  @param  SpdmContext                  A pointer to the SPDM context.
  @param  SessionInfo                  The session info of an SPDM session.
  @param  Hmac                         The buffer to store the PSK exchange HMAC.

  @retval TRUE  PSK exchange HMAC is generated.
  @retval FALSE PSK exchange HMAC is not generated.
**/
BOOLEAN
SpdmResponderGeneratePskExchangeHmac (
  IN  SPDM_DEVICE_CONTEXT       *SpdmContext,
  IN  SPDM_SESSION_INFO         *SessionInfo,
  OUT UINT8                     *Hmac
  )
{
  UINT8                         HmacData[MAX_HASH_SIZE];
  UINT32                        HashSize;
  LARGE_MANAGED_BUFFER          THCurr;

  InitManagedBuffer (&THCurr, MAX_SPDM_MESSAGE_BUFFER_SIZE);

  HashSize = GetSpdmHashSize (SpdmContext);

  DEBUG((DEBUG_INFO, "Calc MessageA Data :\n"));
  InternalDumpHex (GetManagedBuffer(&SpdmContext->Transcript.MessageA), GetManagedBufferSize(&SpdmContext->Transcript.MessageA));

  DEBUG((DEBUG_INFO, "Calc MessageK Data :\n"));
  InternalDumpHex (GetManagedBuffer(&SessionInfo->SessionTranscript.MessageK), GetManagedBufferSize(&SessionInfo->SessionTranscript.MessageK));

  AppendManagedBuffer (&THCurr, GetManagedBuffer(&SpdmContext->Transcript.MessageA), GetManagedBufferSize(&SpdmContext->Transcript.MessageA));
  AppendManagedBuffer (&THCurr, GetManagedBuffer(&SessionInfo->SessionTranscript.MessageK), GetManagedBufferSize(&SessionInfo->SessionTranscript.MessageK));

  ASSERT(SessionInfo->HashSize != 0);
  SpdmHmacAll (SpdmContext, GetManagedBuffer(&THCurr), GetManagedBufferSize(&THCurr), SessionInfo->HandshakeSecret.ResponseFinishedKey, SessionInfo->HashSize, HmacData);
  DEBUG((DEBUG_INFO, "Calc THCurr Hmac - "));
  InternalDumpData (HmacData, HashSize);
  DEBUG((DEBUG_INFO, "\n"));

  CopyMem (Hmac, HmacData, HashSize);

  return TRUE;
}

/**
  Process the SPDM PSK_EXCHANGE request and return the response.

  @param  SpdmContext                  A pointer to the SPDM context.
  @param  RequestSize                  Size in bytes of the request data.
  @param  Request                      A pointer to the request data.
  @param  ResponseSize                 Size in bytes of the response data.
                                       On input, it means the size in bytes of response data buffer.
                                       On output, it means the size in bytes of copied response data buffer if RETURN_SUCCESS is returned,
                                       and means the size in bytes of desired response data buffer if RETURN_BUFFER_TOO_SMALL is returned.
  @param  Response                     A pointer to the response data.

  @retval RETURN_SUCCESS               The request is processed and the response is returned.
  @retval RETURN_BUFFER_TOO_SMALL      The buffer is too small to hold the data.
  @retval RETURN_DEVICE_ERROR          A device error occurs when communicates with the device.
  @retval RETURN_SECURITY_VIOLATION    Any verification fails.
**/
RETURN_STATUS
EFIAPI
SpdmGetResponsePskExchange (
  IN     VOID                 *Context,
  IN     UINTN                RequestSize,
  IN     VOID                 *Request,
  IN OUT UINTN                *ResponseSize,
     OUT VOID                 *Response
  )
{
  SPDM_PSK_EXCHANGE_REQUEST     *SpdmRequest;
  SPDM_PSK_EXCHANGE_RESPONSE    *SpdmResponse;
  BOOLEAN                       Result;
  UINT8                         SlotNum;
  UINT32                        SessionId;
  UINT32                        HashSize;
  UINT32                        HmacSize;
  UINT8                         *Ptr;
  SPDM_SESSION_INFO             *SessionInfo;
  UINTN                         TotalSize;
  SPDM_DEVICE_CONTEXT           *SpdmContext;
  UINT16                        ReqSessionId;
  UINT16                        RspSessionId;
  RETURN_STATUS                 Status;
  UINTN                         OpaquePskExchangeRspSize;

  SpdmContext = Context;
  SpdmRequest = Request;

  if (((SpdmContext->ConnectionInfo.Capability.Flags & SPDM_GET_CAPABILITIES_REQUEST_FLAGS_MUT_AUTH_CAP) != 0) &&
      (SpdmContext->EncapContext.ErrorState != SPDM_STATUS_SUCCESS)) {
    DEBUG((DEBUG_INFO, "SpdmGetResponsePskExchange fail due to Mutual Auth fail\n"));
    SpdmGenerateErrorResponse (SpdmContext, SPDM_ERROR_CODE_INVALID_REQUEST, 0, ResponseSize, Response);
    return RETURN_SUCCESS;
  }

  SlotNum = SpdmRequest->Header.Param2;
  if (SlotNum >= SpdmContext->LocalContext.SlotCount) {
    SpdmGenerateErrorResponse (SpdmContext, SPDM_ERROR_CODE_INVALID_REQUEST, 0, ResponseSize, Response);
    return RETURN_SUCCESS;
  }

  HashSize = GetSpdmHashSize (SpdmContext);
  HmacSize = GetSpdmHashSize (SpdmContext);

  if (RequestSize < sizeof(SPDM_PSK_EXCHANGE_REQUEST)) {
    SpdmGenerateErrorResponse (SpdmContext, SPDM_ERROR_CODE_INVALID_REQUEST, 0, ResponseSize, Response);
    return RETURN_SUCCESS;
  }
  if (RequestSize < sizeof(SPDM_PSK_EXCHANGE_REQUEST) +
                    SpdmRequest->PSKHintLength +
                    SpdmRequest->RequesterContextLength +
                    SpdmRequest->OpaqueLength) {
    SpdmGenerateErrorResponse (SpdmContext, SPDM_ERROR_CODE_INVALID_REQUEST, 0, ResponseSize, Response);
    return RETURN_SUCCESS;
  }
  RequestSize = sizeof(SPDM_PSK_EXCHANGE_REQUEST) +
                SpdmRequest->PSKHintLength +
                SpdmRequest->RequesterContextLength +
                SpdmRequest->OpaqueLength;

  Ptr = (UINT8 *)Request + sizeof(SPDM_PSK_EXCHANGE_REQUEST) + SpdmRequest->PSKHintLength + SpdmRequest->RequesterContextLength;
  Status = SpdmProcessOpaqueDataSupportedVersionData (SpdmContext, SpdmRequest->OpaqueLength, Ptr);
  if (RETURN_ERROR(Status)) {
    SpdmGenerateErrorResponse (SpdmContext, SPDM_ERROR_CODE_INVALID_REQUEST, 0, ResponseSize, Response);
    return RETURN_SUCCESS;
  }

  OpaquePskExchangeRspSize = SpdmGetOpaqueDataVersionSelectionDataSize (SpdmContext);
  TotalSize = sizeof(SPDM_PSK_EXCHANGE_RESPONSE) +
              HashSize +
              DEFAULT_CONTEXT_LENGTH +
              OpaquePskExchangeRspSize +
              HmacSize;

  ASSERT (*ResponseSize >= TotalSize);
  *ResponseSize = TotalSize;
  ZeroMem (Response, *ResponseSize);
  SpdmResponse = Response;

  SpdmResponse->Header.SPDMVersion = SPDM_MESSAGE_VERSION_11;
  SpdmResponse->Header.RequestResponseCode = SPDM_PSK_EXCHANGE_RSP;
  SpdmResponse->Header.Param1 = 0;

  ReqSessionId = SpdmRequest->ReqSessionID;
  RspSessionId = SpdmAllocateRspSessionId (SpdmContext);
  SessionId = (ReqSessionId << 16) | RspSessionId;
  SessionInfo = SpdmAssignSessionId (SpdmContext, SessionId);
  if (SessionInfo == NULL) {
    SpdmGenerateErrorResponse (SpdmContext, SPDM_ERROR_CODE_INVALID_REQUEST, 0, ResponseSize, Response);
    return RETURN_SUCCESS;
  }
  SessionInfo->UsePsk = TRUE;

  AppendManagedBuffer (&SessionInfo->SessionTranscript.MessageK, Request, RequestSize);

  SpdmResponse->RspSessionID = RspSessionId;
  SpdmResponse->Reserved = 0;

  SpdmResponse->ResponderContextLength = DEFAULT_CONTEXT_LENGTH;
  SpdmResponse->OpaqueLength = (UINT16)OpaquePskExchangeRspSize;

  Ptr = (VOID *)(SpdmResponse + 1);
  
  Result = SpdmResponderCalculateMeasurementSummaryHash (SpdmContext, SpdmRequest->Header.Param1, Ptr);
  if (!Result) {
    SpdmFreeSessionId (SpdmContext, SessionId);
    SpdmGenerateErrorResponse (SpdmContext, SPDM_ERROR_CODE_INVALID_REQUEST, 0, ResponseSize, Response);
    return RETURN_SUCCESS;
  }
  Ptr += HashSize;
  
  SpdmGetRandomNumber (DEFAULT_CONTEXT_LENGTH, Ptr);
  Ptr += DEFAULT_CONTEXT_LENGTH;

  Status = SpdmBuildOpaqueDataVersionSelectionData (SpdmContext, &OpaquePskExchangeRspSize, Ptr);
  ASSERT_RETURN_ERROR(Status);
  Ptr += OpaquePskExchangeRspSize;

  AppendManagedBuffer (&SessionInfo->SessionTranscript.MessageK, SpdmResponse, (UINTN)Ptr - (UINTN)SpdmResponse);
  SpdmGenerateSessionHandshakeKey (SpdmContext, SessionId, FALSE);
  
  Result = SpdmResponderGeneratePskExchangeHmac (SpdmContext, SessionInfo, Ptr);
  if (!Result) {
    SpdmFreeSessionId (SpdmContext, SessionId);
    SpdmGenerateErrorResponse (SpdmContext, SPDM_ERROR_CODE_UNSUPPORTED_REQUEST, SPDM_PSK_EXCHANGE_RSP, ResponseSize, Response);
    return RETURN_SUCCESS;
  }
  AppendManagedBuffer (&SessionInfo->SessionTranscript.MessageK, Ptr, HmacSize);
  Ptr += HmacSize;

  SessionInfo->SessionState = SpdmStateHandshaking;

  return RETURN_SUCCESS;
}


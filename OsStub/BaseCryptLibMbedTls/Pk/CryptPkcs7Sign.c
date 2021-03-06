/** @file
  PKCS#7 SignedData Sign Wrapper and PKCS#7 SignedData Verification Wrapper
  Implementation over mbedtls.

  RFC 8422 - Elliptic Curve Cryptography (ECC) Cipher Suites
  FIPS 186-4 - Digital Signature Standard (DSS)

Copyright (c) 2020, Intel Corporation. All rights reserved.<BR>
SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "CryptPkcs7Internal.h"
#include <mbedtls/ecdh.h>

///
/// Enough to store any signature generated by PKCS7
///
#define   MAX_SIGNATURE_SIZE      1024

STATIC
UINTN
EFIAPI
AsciiStrLen (
  IN      CONST CHAR8               *String
  )
{
  UINTN                             Length;

  ASSERT (String != NULL);
  if (String == NULL) {
    return 0;
  }

  for (Length = 0; *String != '\0'; String++, Length++) {
    ;
  }
  return Length;
}

STATIC
INT32
MbedTlsPkcs7WriteDigestAlgorithm (
  UINT8             **P,
  UINT8             *Start,
  mbedtls_md_type_t DigestType
  )
{
  UINT8     *OidPtr;
  INTN      OidLen;
  INT32     Ret;
  Ret = mbedtls_oid_get_oid_by_md (DigestType, &OidPtr, &OidLen);
  if (Ret == 0) {
    return mbedtls_asn1_write_oid (P, Start, OidPtr, OidLen);
  }
  return 0;
}

STATIC
INT32
MbedTlsPkcs7WriteDigestAlgorithmSet (
  UINT8             **P,
  UINT8             *Start,
  mbedtls_md_type_t *DigestTypes,
  INTN              Count
  )
{
  INTN    Idx;
  INT32   Len;
  INT32   ret;

  Len = 0;
  MBEDTLS_ASN1_CHK_ADD (Len, mbedtls_asn1_write_null (P, Start));

  for (Idx = 0; Idx < Count; Idx++) {
    MBEDTLS_ASN1_CHK_ADD (Len,
    MbedTlsPkcs7WriteDigestAlgorithm (P, Start, DigestTypes[Idx]));
  }

  MBEDTLS_ASN1_CHK_ADD (Len, mbedtls_asn1_write_len (P, Start, (UINTN)Len));

  MBEDTLS_ASN1_CHK_ADD (
    Len, mbedtls_asn1_write_tag (
      P, Start, (MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE))
  );

  MBEDTLS_ASN1_CHK_ADD (Len, mbedtls_asn1_write_len (P, Start, (UINTN)Len));

  MBEDTLS_ASN1_CHK_ADD (
    Len, mbedtls_asn1_write_tag (
      P, Start, (MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SET))
  );

  return Len;
}

/**
   ContentInfo ::= SEQUENCE {
        contentType ContentType,
        content
                [0] EXPLICIT ANY DEFINED BY contentType OPTIONAL }
 **/
STATIC
INT32
MbedTlsPkcs7WriteContentInfo (
  UINT8 **P,
  UINT8 *Start,
  UINT8 *Content,
  INTN  ContentLen
  )
{
  INT32 ret;
  INT32 Len;
  Len = 0;
  if (Content != NULL) {
    MBEDTLS_ASN1_CHK_ADD (Len, mbedtls_asn1_write_octet_string (P, Start, Content, ContentLen));
  }

  MBEDTLS_ASN1_CHK_ADD (Len, mbedtls_asn1_write_oid (
    P, Start, MBEDTLS_OID_PKCS7_DATA, sizeof (MBEDTLS_OID_PKCS7_DATA) - 1));

  MBEDTLS_ASN1_CHK_ADD (Len, mbedtls_asn1_write_len (P, Start, Len));

  MBEDTLS_ASN1_CHK_ADD (Len, mbedtls_asn1_write_tag (P, Start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE));

  return Len;
}

/**
   certificates :: SET OF ExtendedCertificateOrCertificate,
   ExtendedCertificateOrCertificate ::= CHOICE {
        certificate Certificate -- x509,
        extendedCertificate[0] IMPLICIT ExtendedCertificate }
 **/
STATIC
INT32
MbedTlsPkcs7WriteCertificates (
  UINT8             **P,
  UINT8             *Start,
  mbedtls_x509_crt  *Cert,
  mbedtls_x509_crt  *OtherCerts
  )
{
  INT32         ret;
  INT32         Len;
  mbedtls_x509_crt  *TmpCert;
  Len = 0;

  /// Write OtherCerts
  TmpCert = OtherCerts;
  while (TmpCert != NULL) {
    MBEDTLS_ASN1_CHK_ADD (Len, mbedtls_asn1_write_raw_buffer (P, Start, TmpCert->raw.p, TmpCert->raw.len));
    TmpCert = TmpCert->next;
  }

  /// Write Cert
  MBEDTLS_ASN1_CHK_ADD (Len, mbedtls_asn1_write_raw_buffer (P, Start, Cert->raw.p, Cert->raw.len));

  /// Write NextContext
  MBEDTLS_ASN1_CHK_ADD (Len, mbedtls_asn1_write_len (P, Start, Len));
  MBEDTLS_ASN1_CHK_ADD (Len, mbedtls_asn1_write_tag (P, Start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_CONTEXT_SPECIFIC));
  return Len;
}

STATIC
INT32
MbedTlsPkcs7WriteInt (
  UINT8                 **P,
  UINT8                 *Start,
  UINT8                 *SerialRaw,
  INTN                  SerialRawLen
  )
{
  INT32                 ret;
  UINT8                 *Ptr;
  INT32                 Len;

  Len = 0;
  Ptr = SerialRaw + SerialRawLen;
  while (Ptr > SerialRaw) {
    *--(*P) = *--Ptr;
    Len++;
  }
  MBEDTLS_ASN1_CHK_ADD(Len, mbedtls_asn1_write_len(P, Start, Len ));
  MBEDTLS_ASN1_CHK_ADD(Len, mbedtls_asn1_write_tag(P, Start, MBEDTLS_ASN1_INTEGER));

  return Len;
}

STATIC
INT32
MbedTlsPkcs7WriteIssuerAndSerialNumber (
  UINT8                 **P,
  UINT8                 *Start,
  UINT8                 *Serial,
  INTN                  SerialLen,
  UINT8                 *IssuerRaw,
  INTN                  IssuerRawLen
  )
{
  INT32     ret;
  INT32     Len;
  Len = 0;
  MBEDTLS_ASN1_CHK_ADD (Len, MbedTlsPkcs7WriteInt (P, Start, Serial, SerialLen));

  MBEDTLS_ASN1_CHK_ADD (Len, mbedtls_asn1_write_raw_buffer (P, Start, IssuerRaw, IssuerRawLen));

  MBEDTLS_ASN1_CHK_ADD (Len, mbedtls_asn1_write_len (P, Start, Len));
  MBEDTLS_ASN1_CHK_ADD (Len, mbedtls_asn1_write_tag (P, Start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE));

  return Len;
}

/**
   SignerInfo ::= SEQUENCE {
        version Version;
        issuerAndSerialNumber   IssuerAndSerialNumber,
        digestAlgorithm DigestAlgorithmIdentifier,
        authenticatedAttributes
                [0] IMPLICIT Attributes OPTIONAL,
        digestEncryptionAlgorithm DigestEncryptionAlgorithmIdentifier,
        encryptedDigest EncryptedDigest,
        unauthenticatedAttributes
                [1] IMPLICIT Attributes OPTIONAL,
 **/
STATIC
INT32
MbedTlsPkcs7WriteSignerInfo (
  UINT8                 **P,
  UINT8                 *Start,
  MbedtlsPkcs7SignerInfo *SignerInfo
  )
{
  INT32                 ret;
  INT32                 Len;
  Len = 0;

  MBEDTLS_ASN1_CHK_ADD (Len, mbedtls_asn1_write_octet_string (P, Start, SignerInfo->Sig.p, SignerInfo->Sig.len));

  MBEDTLS_ASN1_CHK_ADD (Len, mbedtls_asn1_write_algorithm_identifier (P, Start, SignerInfo->SigAlgIdentifier.p, SignerInfo->SigAlgIdentifier.len, 0));

  MBEDTLS_ASN1_CHK_ADD (Len, mbedtls_asn1_write_algorithm_identifier (P, Start, SignerInfo->AlgIdentifier.p, SignerInfo->AlgIdentifier.len, 0));

  MBEDTLS_ASN1_CHK_ADD (Len, MbedTlsPkcs7WriteIssuerAndSerialNumber (P, Start, SignerInfo->Serial.p, SignerInfo->Serial.len, SignerInfo->IssuerRaw.p, SignerInfo->IssuerRaw.len));

  MBEDTLS_ASN1_CHK_ADD (Len, mbedtls_asn1_write_int (P, Start, SignerInfo->Version));

  MBEDTLS_ASN1_CHK_ADD (Len, mbedtls_asn1_write_len (P, Start, Len));
  MBEDTLS_ASN1_CHK_ADD (Len, mbedtls_asn1_write_tag (P, Start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE));

  return Len;
}

STATIC
INT32
MbedTlsPkcs7WriteSignersInfoSet (
  UINT8                   **P,
  UINT8                   *Start,
  MbedtlsPkcs7SignerInfo  *SignersSet
  )
{
  MbedtlsPkcs7SignerInfo  *SignerInfo;
  INT32                   ret;
  INT32                   Len;

  SignerInfo = SignersSet;
  Len = 0;

  while (SignerInfo != NULL) {

    MBEDTLS_ASN1_CHK_ADD (Len, MbedTlsPkcs7WriteSignerInfo (P, Start, SignerInfo));
    // move to next
    SignerInfo = SignerInfo->Next;
  }

  MBEDTLS_ASN1_CHK_ADD (Len, mbedtls_asn1_write_len (P, Start, Len));
  MBEDTLS_ASN1_CHK_ADD (Len, mbedtls_asn1_write_tag (P, Start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SET));

  return Len;
}

/** Signed Data Type
    SignedData ::= SEQUENCE {
    version Version,
    digestAlgorithms DigestAlgorithmIdentifiers,
    contentInfo ContentInfo,
    certificates
        [0] IMPLICIT ExtendedCertificatesAndCertificates
          OPTIONAL,
    crls
      [1] IMPLICIT CertificateRevocationLists OPTIONAL,
    signerInfos SignerInfos }

  DigestAlgorithmIdentifiers ::=
      SET OF DigestAlgorithmIdentifier

  SignerInfos ::= SET OF SignerInfo
**/
STATIC
INT32
MbedTlsPkcs7WriteDer (
    UINT8         **P,
    UINT8         *Start,
    MbedtlsPkcs7  *Pkcs7
  )
{
  INT32       ret;
  INT32       Len;
  mbedtls_md_type_t DigestAlg[1];
  DigestAlg[0] = MBEDTLS_MD_SHA256;
  Len = 0;

  MBEDTLS_ASN1_CHK_ADD (Len, MbedTlsPkcs7WriteSignersInfoSet (P, Start, &(Pkcs7->SignedData.SignerInfos)));

  MBEDTLS_ASN1_CHK_ADD (Len, MbedTlsPkcs7WriteCertificates (P, Start, &(Pkcs7->SignedData.Certificates), Pkcs7->SignedData.Certificates.next));

  MBEDTLS_ASN1_CHK_ADD (Len, MbedTlsPkcs7WriteContentInfo (P, Start, NULL, 0));

  MBEDTLS_ASN1_CHK_ADD (Len, MbedTlsPkcs7WriteDigestAlgorithmSet (P, Start, DigestAlg, 1));

  MBEDTLS_ASN1_CHK_ADD (Len, mbedtls_asn1_write_int (P, Start, Pkcs7->SignedData.Version));

  MBEDTLS_ASN1_CHK_ADD (Len, mbedtls_asn1_write_len (P, Start, Len));
  MBEDTLS_ASN1_CHK_ADD (Len, mbedtls_asn1_write_tag (P, Start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE));

  return Len;
}

/**
  Creates a PKCS#7 signedData as described in "PKCS #7: Cryptographic Message
  Syntax Standard, version 1.5". This interface is only intended to be used for
  application to perform PKCS#7 functionality validation.

  If this interface is not supported, then return FALSE.

  @param[in]  PrivateKey       Pointer to the PEM-formatted private key data for
                               data signing.
  @param[in]  PrivateKeySize   Size of the PEM private key data in bytes.
  @param[in]  KeyPassword      NULL-terminated passphrase used for encrypted PEM
                               key data.
  @param[in]  InData           Pointer to the content to be signed.
  @param[in]  InDataSize       Size of InData in bytes.
  @param[in]  SignCert         Pointer to signer's DER-encoded certificate to sign with.
  @param[in]  OtherCerts       Pointer to an optional additional set of certificates to
                               include in the PKCS#7 signedData (e.g. any intermediate
                               CAs in the chain).
  @param[out] SignedData       Pointer to output PKCS#7 signedData. It's caller's
                               responsibility to free the buffer with FreePool().
  @param[out] SignedDataSize   Size of SignedData in bytes.

  @retval     TRUE             PKCS#7 data signing succeeded.
  @retval     FALSE            PKCS#7 data signing failed.
  @retval     FALSE            This interface is not supported.

**/
BOOLEAN
EFIAPI
Pkcs7Sign (
  IN   CONST UINT8  *PrivateKey,
  IN   UINTN        PrivateKeySize,
  IN   CONST UINT8  *KeyPassword,
  IN   UINT8        *InData,
  IN   UINTN        InDataSize,
  IN   UINT8        *SignCert,
  IN   UINT8        *OtherCerts      OPTIONAL,
  OUT  UINT8        **SignedData,
  OUT  UINTN        *SignedDataSize
  )
{
  BOOLEAN             Status;
  INT32               Ret;
  mbedtls_pk_context  Pkey;
  UINT8               HashValue[SHA256_DIGEST_SIZE];
  UINT8               Signature[MAX_SIGNATURE_SIZE];
  INTN                SignatureLen;
  UINT8               *NewPrivateKey;
  mbedtls_x509_crt    *Crt;

  MbedtlsPkcs7        Pkcs7;
  MbedtlsPkcs7SignerInfo SignerInfo;
  UINT8               *Buffer;
  INTN                BufferSize;
  UINT8               *P;
  INT32               Len;

  BufferSize = 4096;

  SignatureLen = MAX_SIGNATURE_SIZE;
  Crt = (mbedtls_x509_crt *)SignCert;

  NewPrivateKey = NULL;
  if (PrivateKey[PrivateKeySize - 1] != 0) {
    NewPrivateKey = AllocatePool (PrivateKeySize + 1);
    if (NewPrivateKey == NULL) {
      return FALSE;
    }
    CopyMem (NewPrivateKey, PrivateKey, PrivateKeySize);
    NewPrivateKey[PrivateKeySize] = 0;
  }

  mbedtls_pk_init (&Pkey);
  Ret = mbedtls_pk_parse_key (
    &Pkey, NewPrivateKey, PrivateKeySize + 1,
    KeyPassword, KeyPassword == NULL ? 0 : AsciiStrLen (KeyPassword)
  );
  if (Ret != 0) {
    Status = FALSE;
    goto Cleanup;
  }

  /// Calculate InData Digest
  ZeroMem (HashValue, SHA256_DIGEST_SIZE);
  Status = Sha256HashAll (InData, InDataSize, HashValue);
  if (!Status) {
    goto Cleanup;
  }

  /// Pk Sign
  ZeroMem (Signature, MAX_SIGNATURE_SIZE);
  Ret = mbedtls_pk_sign (
    &Pkey, MBEDTLS_MD_SHA256, HashValue, SHA256_DIGEST_SIZE,
    Signature, &SignatureLen, myrand, NULL);
  if (Ret != 0) {
    Status = FALSE;
    goto Cleanup;
  }

  ZeroMem (&Pkcs7, sizeof(MbedtlsPkcs7));
  Pkcs7.SignedData.Version = 1;

  Crt->next = (mbedtls_x509_crt *)OtherCerts;
  Pkcs7.SignedData.Certificates = *Crt;

  SignerInfo.Next = NULL;
  SignerInfo.Sig.p = Signature;
  SignerInfo.Sig.len = SignatureLen;
  SignerInfo.Version = 1;
  SignerInfo.AlgIdentifier.p = MBEDTLS_OID_DIGEST_ALG_SHA256;
  SignerInfo.AlgIdentifier.len = sizeof (MBEDTLS_OID_DIGEST_ALG_SHA256) - 1;
  if (mbedtls_pk_get_type (&Pkey) == MBEDTLS_PK_RSA) {
    SignerInfo.SigAlgIdentifier.p = MBEDTLS_OID_PKCS1_RSA;
    SignerInfo.SigAlgIdentifier.len = sizeof (MBEDTLS_OID_PKCS1_RSA) - 1;
  } else {
    mbedtls_oid_get_oid_by_sig_alg (MBEDTLS_PK_ECDSA, MBEDTLS_MD_SHA256, &SignerInfo.SigAlgIdentifier.p, &SignerInfo.SigAlgIdentifier.len);
  }
  SignerInfo.Serial = ((mbedtls_x509_crt *)SignCert)->serial;
  SignerInfo.IssuerRaw = ((mbedtls_x509_crt *)SignCert)->issuer_raw;
  Pkcs7.SignedData.SignerInfos = SignerInfo;

  Buffer = AllocateZeroPool (BufferSize);
  if (Buffer == NULL) {
    Status = FALSE;
    goto Cleanup;
  }
  P = Buffer + BufferSize;
  Len = MbedTlsPkcs7WriteDer (&P, Buffer, &Pkcs7);

  /// Enlarge buffer if buffer is too small
  while (Len < 0 && Len == MBEDTLS_ERR_ASN1_BUF_TOO_SMALL) {
    BufferSize = BufferSize * 2;
    P = Buffer + BufferSize;
    FreePool (Buffer);
    Buffer = AllocateZeroPool (BufferSize);
    if (Buffer == NULL) {
      Status = FALSE;
      goto Cleanup;
    }
    P = Buffer + BufferSize;
    Len = MbedTlsPkcs7WriteDer (&P, Buffer, &Pkcs7);
  }
  if (Len <= 0) {
    Status = FALSE;
    goto Cleanup;
  }

  *SignedData = AllocateZeroPool(Len);
  *SignedDataSize = Len;
  CopyMem (*SignedData, P, Len);
  Status = TRUE;

Cleanup:
  mbedtls_pk_free (&Pkey);

  if (NewPrivateKey != NULL) {
    FreePool (NewPrivateKey);
  }

  if (Buffer != NULL) {
    FreePool (Buffer);
  }
  return Status;
}

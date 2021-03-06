/**
@file
UEFI OS based application.

Copyright (c) 2020, Intel Corporation. All rights reserved.<BR>
SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "SpdmUnitTest.h"
#include <SpdmResponderLibInternal.h>

int SpdmResponderVersionTestMain (void);
int SpdmResponderCapabilityTestMain (void);
int SpdmResponderAlgorithmTestMain (void);
int SpdmResponderDigestTestMain (void);
int SpdmResponderCertificateTestMain (void);
int SpdmResponderChallengeAuthTestMain (void);
int SpdmResponderMeasurementTestMain (void);
int SpdmResponderKeyExchangeTestMain (void);

int main(void) {
  SpdmResponderVersionTestMain ();

  SpdmResponderCapabilityTestMain ();

  SpdmResponderAlgorithmTestMain ();

  SpdmResponderDigestTestMain ();

  SpdmResponderCertificateTestMain ();

  SpdmResponderChallengeAuthTestMain ();

  SpdmResponderMeasurementTestMain ();

  SpdmResponderKeyExchangeTestMain ();
  return 0;
}

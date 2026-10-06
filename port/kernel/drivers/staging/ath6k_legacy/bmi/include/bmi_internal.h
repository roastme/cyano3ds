//------------------------------------------------------------------------------
// Copyright (c) 2004-2010 Atheros Communications Inc.
// All rights reserved.
// 
//
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//
//
//------------------------------------------------------------------------------
//==============================================================================
//
// Author(s): ="Atheros"
//==============================================================================
#ifndef BMI_INTERNAL_H
#define BMI_INTERNAL_H

#include "a_config.h"
#include "athdefs.h"
#include "a_osapi.h"
#define ATH_MODULE_NAME bmi
#include "a_debug.h"
#include "hw/mbox_host_reg.h"
#include "bmi_msg.h"

#define ATH_DEBUG_BMI  ATH_DEBUG_MAKE_MODULE_MASK(0)


#define BMI_COMMUNICATION_TIMEOUT       100000

/* N3DS_BMI_EXECUTE_RESPONSE_BOUND: BMI_EXECUTE's response wait
 * (bmiBufferReceive() called with want_timeout=false) is the one BMI/HTC
 * step in the whole bring-up chain the vendor code left genuinely
 * unbounded -- see the fix at its call site in bmi.c for the full
 * explanation of why an unbounded wait here becomes an unrecoverable
 * Wi-Fi enable/disable hang. 10s is generous next to the underlying 6s
 * ath6k_legacy HIF synchronous-request bound (hif.c:
 * HIF_SYNC_REQUEST_TIMEOUT_MS) yet stays safely inside this HAL's own 30s
 * wifi_load_driver() ceiling (libhardware_legacy/wifi/wifi.c:
 * WIFI_DRIVER_INTERFACE_RETRIES). */
#define BMI_EXECUTE_RESPONSE_TIMEOUT_MS 10000

/* ------ Global Variable Declarations ------- */
static bool bmiDone;

int
bmiBufferSend(struct hif_device *device,
              u8 *buffer,
              u32 length);

int
bmiBufferReceive(struct hif_device *device,
                 u8 *buffer,
                 u32 length,
                 bool want_timeout);

#endif

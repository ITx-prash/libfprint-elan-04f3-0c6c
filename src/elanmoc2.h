/*
 * Driver for ELAN Match-On-Chip 2 (ARM-M4) — 04f3:0c6c
 *
 * Copyright (C) 2021-2023 Davide Depau <davide@depau.eu>
 * Copyright (C) 2026 Prashant Adhikari
 *
 * Originally written by Davide Depau for the ELAN 04f3:0c4c sensor.
 * Protocol reverse-engineered from captures of the official Windows driver,
 * with a multiplatform Python prototype:
 * https://github.com/depau/Elan-Fingerprint-0c4c-PoC/
 *
 * Adapted for the ELAN 04f3:0c6c sensor by Prashant Adhikari (2026):
 * - Independent USB traffic analysis via Wireshark captures of the
 *   official Windows driver on the 04f3:0c6c hardware
 * - Discovery and correction of 0-indexed hardware slot addressing
 *   specific to this sensor variant (differs from 0c4c behavior)
 * - Removal of code irrelevant to the 04f3:0c6c protocol
 * - ARM-M4 specific pre-commit slot marker sequence (00 10 <slot>)
 *   identified and implemented from capture data
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#pragma once

// Stdlib includes
#include <stdbool.h>

// Library includes
#include <libusb.h>

// Local includes
#include "fpi-device.h"
#include "fpi-ssm.h"

#define ELANMOC2_DRIVER_FULLNAME "ELAN Match-on-Chip 2"
#define ELANMOC2_VEND_ID 0x04f3

#define ELANMOC2_ENROLL_TIMES 15
#define ELANMOC2_CMD_MAX_LEN 2
#define ELANMOC2_MAX_PRINTS 10
#define ELANMOC2_MAX_RETRIES 3

// USB parameters
#define ELANMOC2_EP_CMD_OUT (0x1 | FPI_USB_ENDPOINT_OUT)
#define ELANMOC2_EP_CMD_IN (0x3 | FPI_USB_ENDPOINT_IN)
#define ELANMOC2_EP_MOC_CMD_IN (0x4 | FPI_USB_ENDPOINT_IN)
#define ELANMOC2_USB_SEND_TIMEOUT 10000
#define ELANMOC2_USB_RECV_TIMEOUT 0

// Response codes
#define ELANMOC2_RESP_MOVE_DOWN 0x41
#define ELANMOC2_RESP_MOVE_RIGHT 0x42
#define ELANMOC2_RESP_MOVE_UP 0x43
#define ELANMOC2_RESP_MOVE_LEFT 0x44
#define ELANMOC2_RESP_MAX_ENROLLED_REACHED 0xdd
#define ELANMOC2_RESP_SENSOR_DIRTY 0xfb
#define ELANMOC2_RESP_NOT_ENROLLED 0xfd
#define ELANMOC2_RESP_NOT_ENOUGH_SURFACE 0xfe

// Subtract the 2-byte header
#define ELANMOC2_USER_ID_MAX_LEN (cmd_finger_info.in_len - 2)

G_DECLARE_FINAL_TYPE (FpiDeviceElanMoC2, fpi_device_elanmoc2, FPI, DEVICE_ELANMOC2, FpDevice)

typedef struct elanmoc2_cmd
{
  unsigned char  cmd[ELANMOC2_CMD_MAX_LEN];
  gboolean       is_single_byte_command;
  int            out_len;
  int            in_len;
  int            ep_in;
  gboolean       is_cancellable;
  gboolean       ssm_not_required;
} Elanmoc2Cmd;


// Cancellable commands

static const Elanmoc2Cmd cmd_identify = {
  .cmd = {0xff, 0x03},
  .out_len = 3,
  .in_len = 2,
  .ep_in = ELANMOC2_EP_MOC_CMD_IN,
  .is_cancellable = TRUE,
};

static const Elanmoc2Cmd cmd_enroll = {
  .cmd = {0xff, 0x01},
  .out_len = 7,
  .in_len = 2,
  .ep_in = ELANMOC2_EP_MOC_CMD_IN,
  .is_cancellable = TRUE,
};


// Not cancellable / quick commands

static const Elanmoc2Cmd cmd_get_fw_ver = {
  .cmd = {0x19},
  .is_single_byte_command = TRUE,
  .out_len = 2,
  .in_len = 2,
  .ep_in = ELANMOC2_EP_CMD_IN,
};

static const Elanmoc2Cmd cmd_finger_info = {
  .cmd = {0xff, 0x12},
  .out_len = 4,
  .in_len = 64,
  .ep_in = ELANMOC2_EP_CMD_IN,
};

static const Elanmoc2Cmd cmd_get_enrolled_count = {
  .cmd = {0xff, 0x04},
  .out_len = 3,
  .in_len = 2,
  .ep_in = ELANMOC2_EP_CMD_IN,
};

static const Elanmoc2Cmd cmd_abort = {
  .cmd = {0xff, 0x02},
  .out_len = 3,
  .in_len = 2,
  .ep_in = ELANMOC2_EP_CMD_IN,
  .ssm_not_required = TRUE,
};

static const Elanmoc2Cmd cmd_commit = {
  .cmd = {0xff, 0x11},
  .out_len = 72,
  .in_len = 2,
  .ep_in = ELANMOC2_EP_CMD_IN,
};

static const Elanmoc2Cmd cmd_check_enroll_collision = {
  .cmd = {0xff, 0x10},
  .out_len = 3,
  .in_len = 3,
  .ep_in = ELANMOC2_EP_CMD_IN,
};

/* ARM-M4 specific: sent between collision check and commit.
 * Observed in Windows WBF capture (frame 133): 00 10 <enrolled_num>
 * No response expected (in_len = 0). Buffer built manually (NOT via elanmoc2_prepare_cmd). */
static const Elanmoc2Cmd cmd_arm_m4_commit_slot = {
  .cmd = {0x00, 0x10},
  .out_len = 3,
  .in_len = 0,
  .ep_in = ELANMOC2_EP_CMD_IN,
};

static const Elanmoc2Cmd cmd_delete = {
  .cmd = {0xff, 0x13},
  .out_len = 72,
  .in_len = 2,
  .ep_in = ELANMOC2_EP_CMD_IN,
};

static const Elanmoc2Cmd cmd_wipe_sensor = {
  .cmd = {0xff, 0x99},
  .out_len = 3,
  .in_len = 0,
  .ep_in = ELANMOC2_EP_CMD_IN,
};


enum IdentifyStates {
  IDENTIFY_GET_NUM_ENROLLED,
  IDENTIFY_CHECK_NUM_ENROLLED,
  IDENTIFY_IDENTIFY,
  IDENTIFY_GET_FINGER_INFO,
  IDENTIFY_CHECK_FINGER_INFO,
  IDENTIFY_NUM_STATES
};

enum EnrollStates {
  ENROLL_GET_NUM_ENROLLED,
  ENROLL_CHECK_NUM_ENROLLED,
  ENROLL_EARLY_REENROLL_CHECK,
  ENROLL_GET_ENROLLED_FINGER_INFO,
  ENROLL_ATTEMPT_DELETE,
  ENROLL_CHECK_DELETED,
  ENROLL_WIPE_SENSOR,
  ENROLL_PRIME_SENSOR,
  ENROLL_PRIME_SENSOR_RESPONSE,
  ENROLL_ENROLL,
  ENROLL_CHECK_ENROLLED,
  ENROLL_RETRY_JUMP,
  ENROLL_LATE_REENROLL_CHECK,
  ENROLL_PRE_COMMIT,        /* ARM-M4: checks collision result + sends 00 10 XX */
  ENROLL_COMMIT,            /* sends cmd_commit (72 bytes) */
  ENROLL_CHECK_COMMITTED,
  ENROLL_NUM_STATES
};

enum ClearStorageStates {
  CLEAR_STORAGE_WIPE_SENSOR,
  CLEAR_STORAGE_GET_NUM_ENROLLED,
  CLEAR_STORAGE_CHECK_NUM_ENROLLED,
  CLEAR_STORAGE_NUM_STATES
};

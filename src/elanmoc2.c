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

#define FP_COMPONENT "elanmoc2"

// Library includes
#include <glib.h>
#include <sys/param.h>

// Local includes
#include "drivers_api.h"

#include "elanmoc2.h"

struct _FpiDeviceElanMoC2
{
  FpDevice parent;

  /* Device properties */


  /* USB response data */
  GBytes            *buffer_in;
  const Elanmoc2Cmd *in_flight_cmd;

  /* Command status data */
  FpiSsm      *ssm;
  unsigned int enrolled_num;
  unsigned int enrolled_num_retries;
  unsigned int print_index;
  GPtrArray   *list_result;

  // Enroll
  int      enroll_stage;
  FpPrint *enroll_print;
};

G_DEFINE_TYPE (FpiDeviceElanMoC2, fpi_device_elanmoc2, FP_TYPE_DEVICE);


static void
elanmoc2_cmd_usb_callback (FpiUsbTransfer *transfer,
                           FpDevice       *device,
                           gpointer        user_data,
                           GError         *error)
{
  FpiDeviceElanMoC2 *self = FPI_DEVICE_ELANMOC2 (device);
  const gboolean short_is_error = GPOINTER_TO_INT (user_data);

  if (self->ssm == NULL)
    {
      if (self->in_flight_cmd == NULL || !self->in_flight_cmd->ssm_not_required)
        fp_warn ("Received USB callback with no ongoing action");

      self->in_flight_cmd = NULL;

      if (error)
        {
          fp_info ("USB callback error: %s", error->message);
          g_error_free (error);
        }
      return;
    }

  if (error)
    {
      fpi_ssm_mark_failed (g_steal_pointer (&self->ssm),
                           g_steal_pointer (&error));
      return;
    }

  if (self->in_flight_cmd != NULL)
    {
      /* Send callback */
      const Elanmoc2Cmd *cmd = g_steal_pointer (&self->in_flight_cmd);

      if (cmd->in_len == 0)
        {
          /* Nothing to receive */
          if (cmd->cmd[0] == 0xff && cmd->cmd[1] == 0x99)
            {
              fpi_ssm_next_state_delayed (self->ssm, 3000);
            }
          else
            {
              fpi_ssm_next_state (self->ssm);
            }
          return;
        }

      FpiUsbTransfer *transfer_in = fpi_usb_transfer_new (device);

      transfer_in->short_is_error = short_is_error;

      fpi_usb_transfer_fill_bulk (transfer_in, cmd->ep_in,
                                  cmd->in_len);

      fpi_usb_transfer_submit (transfer_in,
                               ELANMOC2_USB_RECV_TIMEOUT,
                               cmd->is_cancellable ?
                               fpi_device_get_cancellable (device) : NULL,
                               elanmoc2_cmd_usb_callback,
                               NULL);
    }
  else
    {
      /* Receive callback */
      if (transfer->actual_length > 0 && transfer->buffer[0] != 0x40)
        {
          fpi_ssm_mark_failed (g_steal_pointer (&self->ssm),
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                         "Error receiving data "
                                                         "from sensor"));
        }
      else
        {
          g_assert_null (self->buffer_in);
          self->buffer_in =
            g_bytes_new_take (g_steal_pointer (&(transfer->buffer)),
                              transfer->actual_length);
          fpi_ssm_next_state (self->ssm);
        }
    }
}

static void
elanmoc2_cmd_transceive_full (FpDevice          *device,
                              const Elanmoc2Cmd *cmd,
                              GByteArray        *buffer_out,
                              gboolean           short_is_error
                             )
{
  FpiDeviceElanMoC2 *self = FPI_DEVICE_ELANMOC2 (device);

  g_assert (buffer_out->len == cmd->out_len);
  g_assert_null (self->in_flight_cmd);
  self->in_flight_cmd = cmd;

  g_autoptr(FpiUsbTransfer) transfer_out = fpi_usb_transfer_new (device);
  transfer_out->short_is_error = TRUE;
  fpi_usb_transfer_fill_bulk_full (transfer_out,
                                   ELANMOC2_EP_CMD_OUT,
                                   g_byte_array_steal (buffer_out, NULL),
                                   cmd->out_len,
                                   g_free);

  fpi_usb_transfer_submit (g_steal_pointer (&transfer_out),
                           ELANMOC2_USB_SEND_TIMEOUT,
                           cmd->is_cancellable ?
                           fpi_device_get_cancellable (device) : NULL,
                           elanmoc2_cmd_usb_callback,
                           GINT_TO_POINTER (short_is_error));
}

static void
elanmoc2_cmd_transceive (FpDevice          *device,
                         const Elanmoc2Cmd *cmd,
                         GByteArray        *buffer_out)
{
  elanmoc2_cmd_transceive_full (device, cmd, buffer_out, TRUE);
}

static GByteArray *
elanmoc2_prepare_cmd (FpiDeviceElanMoC2 *self, const Elanmoc2Cmd *cmd)
{


  g_assert (cmd->out_len > 0);

  GByteArray *buffer = g_byte_array_new ();
  g_byte_array_set_size (buffer, cmd->out_len);
  memset (buffer->data, 0, buffer->len);

  buffer->data[0] = 0x40;
  memcpy (&buffer->data[1], cmd->cmd, cmd->is_single_byte_command ? 1 : 2);

  return buffer;
}

static void
elanmoc2_print_set_data (FpPrint      *print,
                         guchar        finger_id,
                         guchar        user_id_len,
                         const guchar *user_id)
{
  fpi_print_set_type (print, FPI_PRINT_RAW);
  fpi_print_set_device_stored (print, TRUE);

  GVariant *user_id_v = g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                                   user_id, user_id_len,
                                                   sizeof (guchar));
  GVariant *fpi_data = g_variant_new ("(y@ay)", finger_id, user_id_v);
  g_object_set (print, "fpi-data", fpi_data, NULL);
}

static FpPrint *
elanmoc2_print_new_with_user_id (FpiDeviceElanMoC2 *self,
                                 guchar             finger_id,
                                 guchar             user_id_len,
                                 const guchar      *user_id)
{
  FpPrint *print = fp_print_new (FP_DEVICE (self));

  elanmoc2_print_set_data (print, finger_id, user_id_len, user_id);
  return g_steal_pointer (&print);
}

static guint
elanmoc2_get_user_id_max_length (FpiDeviceElanMoC2 *self)
{
  return ELANMOC2_USER_ID_MAX_LEN;
}

static GBytes *
elanmoc2_get_user_id_string (FpiDeviceElanMoC2 *self,
                             GBytes            *finger_info_response)
{
  GByteArray *user_id = g_byte_array_new ();

  guint offset = 2;
  guint max_len = 0;
  gsize response_len = g_bytes_get_size (finger_info_response);

  if (response_len > offset)
    max_len = MIN (elanmoc2_get_user_id_max_length (self),
                   (guint) (response_len - offset));

  g_byte_array_set_size (user_id, max_len + 1);

  /* Copy bytes and ensure a trailing NUL for string helpers. */
  const guint8 *data = g_bytes_get_data (finger_info_response, NULL);
  memcpy (user_id->data, &data[offset], max_len);
  user_id->data[max_len] = '\0';

  /* Keep the NUL in memory but return the exact byte length. */
  return g_bytes_new_take (g_byte_array_free (user_id, FALSE), max_len);
}

static FpPrint *
elanmoc2_print_new_from_finger_info (FpiDeviceElanMoC2 *self,
                                     guint8             finger_id,
                                     GBytes            *finger_info_resp)
{
  g_autoptr(GBytes) user_id = elanmoc2_get_user_id_string (self,
                                                           finger_info_resp);
  guint8 user_id_len = g_bytes_get_size (user_id);
  const char *user_id_data = g_bytes_get_data (user_id, NULL);

  if (g_str_has_prefix ( user_id_data, "FP"))
    {
      user_id_len = strnlen (user_id_data, user_id_len);
      fp_info ("Creating new print: finger %d, user id[%d]: %s",
               finger_id,
               user_id_len,
               (char *) user_id_data);
    }
  else
    {
      fp_info ("Creating new print: finger %d, user id[%d]: raw data",
               finger_id,
               user_id_len);
    }

  FpPrint *print =
    elanmoc2_print_new_with_user_id (self,
                                     finger_id,
                                     user_id_len,
                                     (const guint8 *) user_id_data);

  if (!fpi_print_fill_from_user_id (print, (const char *) user_id_data))
    /* Fingerprint matched with on-sensor print, but the on-sensor print was
     * not added by libfprint. Wipe it and report a failure. */
    fp_info ("Finger info not generated by libfprint");
  else
    fp_info ("Finger info with libfprint user ID");

  return g_steal_pointer (&print);
}

static void
elanmoc2_cancel (FpDevice *device)
{
  FpiDeviceElanMoC2 *self = FPI_DEVICE_ELANMOC2 (device);

  fp_info ("Cancelling any ongoing requests");

  g_autoptr(GByteArray) buffer_out = elanmoc2_prepare_cmd (self, &cmd_abort);
  elanmoc2_cmd_transceive (device, &cmd_abort, buffer_out);
}

static void
elanmoc2_open (FpDevice *device)
{
  g_autoptr(GError) error = NULL;
  FpiDeviceElanMoC2 *self;

  if (!g_usb_device_reset (fpi_device_get_usb_device (device), &error))
    return fpi_device_open_complete (device, g_steal_pointer (&error));

  if (!g_usb_device_claim_interface (
        fpi_device_get_usb_device (FP_DEVICE (device)), 0, 0, &error))
    return fpi_device_open_complete (device, g_steal_pointer (&error));

  self = FPI_DEVICE_ELANMOC2 (device);

  fpi_device_open_complete (device, NULL);
}

static void
elanmoc2_close (FpDevice *device)
{
  g_autoptr(GError) error = NULL;

  fp_info ("Closing device");
  elanmoc2_cancel (device);
  g_usb_device_release_interface (fpi_device_get_usb_device (FP_DEVICE (device)),
                                  0, 0, &error);
  fpi_device_close_complete (device, g_steal_pointer (&error));
}

static void
elanmoc2_ssm_completed_callback (FpiSsm *ssm, FpDevice *device, GError *error)
{
  if (error)
    fpi_device_action_error (device, error);
}

static void
elanmoc2_perform_get_num_enrolled (FpiDeviceElanMoC2 *self, FpiSsm *ssm)
{
  FpDevice *device = FP_DEVICE (self);
  g_autoptr(GByteArray) buffer_out = elanmoc2_prepare_cmd (self,
                                                           &cmd_get_enrolled_count);
  if (buffer_out == NULL)
    {
      fpi_ssm_next_state (ssm);
      return;
    }
  elanmoc2_cmd_transceive (device, &cmd_get_enrolled_count, buffer_out);
}

static GError *
elanmoc2_get_num_enrolled_retry_or_error (FpiDeviceElanMoC2 *self,
                                          FpiSsm            *ssm,
                                          int                retry_state)
{
  fp_info ("Device returned no data, retrying");
  if (self->enrolled_num_retries >= ELANMOC2_MAX_RETRIES)
    return fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                     "Device refused to respond to query for "
                                     "number of enrolled fingers");
  self->enrolled_num_retries++;
  fpi_ssm_jump_to_state (ssm, retry_state);
  return NULL;
}

/**
 * elanmoc2_get_finger_error:
 * @self: #FpiDeviceElanMoC2 pointer
 * @out_can_retry: Whether the current action should be retried (out)
 *
 * Checks a command status code and, if an error has occurred, creates a new
 * error object. Returns whether the operation needs to be retried.
 *
 * Returns: #GError if failed, or %NULL
 */
static GError *
elanmoc2_get_finger_error (GBytes *buffer_in, gboolean *out_can_retry)
{
  g_assert_nonnull (buffer_in);
  g_assert (g_bytes_get_size (buffer_in) >= 2);

  const guint8 *data_in = g_bytes_get_data (buffer_in, NULL);

  /* Regular status codes never have the most-significant nibble set;
   * errors do */
  if ((data_in[1] & 0xF0) == 0)
    {
      *out_can_retry = TRUE;
      return NULL;
    }
  switch ((unsigned char) data_in[1])
    {
    case ELANMOC2_RESP_MOVE_DOWN:
      *out_can_retry = TRUE;
      return fpi_device_retry_new_msg (FP_DEVICE_RETRY_CENTER_FINGER,
                                       "Move your finger slightly downwards");

    case ELANMOC2_RESP_MOVE_RIGHT:
      *out_can_retry = TRUE;
      return fpi_device_retry_new_msg (FP_DEVICE_RETRY_CENTER_FINGER,
                                       "Move your finger slightly to the right");

    case ELANMOC2_RESP_MOVE_UP:
      *out_can_retry = TRUE;
      return fpi_device_retry_new_msg (FP_DEVICE_RETRY_CENTER_FINGER,
                                       "Move your finger slightly upwards");

    case ELANMOC2_RESP_MOVE_LEFT:
      *out_can_retry = TRUE;
      return fpi_device_retry_new_msg (FP_DEVICE_RETRY_CENTER_FINGER,
                                       "Move your finger slightly to the left");

    case ELANMOC2_RESP_SENSOR_DIRTY:
      *out_can_retry = TRUE;
      return fpi_device_retry_new_msg (FP_DEVICE_RETRY_REMOVE_FINGER,
                                       "Sensor is dirty or wet");

    case ELANMOC2_RESP_NOT_ENOUGH_SURFACE:
      *out_can_retry = TRUE;
      return fpi_device_retry_new_msg (FP_DEVICE_RETRY_REMOVE_FINGER,
                                       "Press your finger slightly harder on "
                                       "the sensor");

    case ELANMOC2_RESP_NOT_ENROLLED:
      *out_can_retry = FALSE;
      return fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_NOT_FOUND,
                                       "Finger not recognized");

    case ELANMOC2_RESP_MAX_ENROLLED_REACHED:
      *out_can_retry = FALSE;
      return fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_FULL,
                                       "Maximum number of fingers already "
                                       "enrolled");

    default:
      fp_err ("elanmoc2_get_finger_error: Unknown error byte 0x%02x (data_in[0]=0x%02x, len=%zu)", data_in[1], data_in[0], g_bytes_get_size(buffer_in));
      *out_can_retry = FALSE;
      return fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                       "Unknown error");
    }
}

static void
elanmoc2_identify_verify_complete (FpDevice *device, GError *error)
{
  if (fpi_device_get_current_action (device) == FPI_DEVICE_ACTION_IDENTIFY)
    fpi_device_identify_complete (device, error);
  else
    fpi_device_verify_complete (device, error);
}

/**
 * elanmoc2_identify_verify_report:
 * @device: #FpDevice
 * @print: Identified fingerprint
 * @error: Optional error
 *
 * Calls the correct verify or identify report function based on the input data.
 * Returns whether the action should be completed.
 *
 * Returns: Whether to complete the action.
 */
static gboolean
elanmoc2_identify_verify_report (FpDevice *device, FpPrint *print,
                                 GError **error)
{
  if (*error != NULL && (*error)->domain != FP_DEVICE_RETRY)
    return TRUE;

  if (fpi_device_get_current_action (device) == FPI_DEVICE_ACTION_IDENTIFY)
    {
      if (print != NULL)
        {
          GPtrArray * gallery = NULL;
          fpi_device_get_identify_data (device, &gallery);

          for (int i = 0; i < gallery->len; i++)
            {
              FpPrint *to_match = g_ptr_array_index (gallery, i);
              if (fp_print_equal (to_match, print))
                {
                  fp_info ("Identify: finger matches");
                  fpi_device_identify_report (device,
                                              g_steal_pointer (&to_match),
                                              print,
                                              NULL);
                  return TRUE;
                }
            }
          fp_info ("Identify: no match");
        }
      fpi_device_identify_report (device, NULL, print, *error);
      return TRUE;
    }
  else
    {
      FpiMatchResult result = FPI_MATCH_FAIL;
      if (print != NULL)
        {
          FpPrint *to_match = NULL;
          fpi_device_get_verify_data (device, &to_match);
          g_assert_nonnull (to_match);

          if (fp_print_equal (to_match, print))
            {
              fp_info ("Verify: finger matches");
              result = FPI_MATCH_SUCCESS;
            }
          else
            {
              fp_info ("Verify: finger does not match");
              print = NULL;
            }
        }
      fpi_device_verify_report (device, result, print, *error);
      return result != FPI_MATCH_FAIL;
    }
}

static void
elanmoc2_identify_run_state (FpiSsm *ssm, FpDevice *device)
{
  FpiDeviceElanMoC2 *self = FPI_DEVICE_ELANMOC2 (device);

  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) buffer_in = g_steal_pointer (&self->buffer_in);

  const guint8 *data_in =
    buffer_in != NULL ? g_bytes_get_data (buffer_in, NULL) : NULL;
  const gsize data_in_len =
    buffer_in != NULL ? g_bytes_get_size (buffer_in) : 0;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case IDENTIFY_GET_NUM_ENROLLED: {
        elanmoc2_perform_get_num_enrolled (self, ssm);
        break;
      }

    case IDENTIFY_CHECK_NUM_ENROLLED: {
        if (data_in_len == 0)
          {
            error =
              elanmoc2_get_num_enrolled_retry_or_error (self,
                                                        ssm,
                                                        IDENTIFY_GET_NUM_ENROLLED);
            if (error != NULL)
              {
                elanmoc2_identify_verify_complete (device,
                                                   g_steal_pointer (&error));
                fpi_ssm_mark_completed (g_steal_pointer (&self->ssm));
              }
            break;
          }

        g_assert_nonnull (data_in);
        g_assert (data_in_len >= 2);

        self->enrolled_num = data_in[1];

        if (self->enrolled_num == 0)
          {
            fp_info ("No fingers enrolled, no need to identify finger");
            error = NULL;
            elanmoc2_identify_verify_report (device, NULL, &error);
            elanmoc2_identify_verify_complete (device, NULL);
            fpi_ssm_mark_completed (g_steal_pointer (&self->ssm));
            break;
          }
        fpi_ssm_next_state (ssm);
        break;
      }

    case IDENTIFY_IDENTIFY: {
        g_autoptr(GByteArray) buffer_out = elanmoc2_prepare_cmd (self,
                                                                 &cmd_identify);
        if (buffer_out == NULL)
          {
            fpi_ssm_next_state (ssm);
            break;
          }
        elanmoc2_cmd_transceive (device, &cmd_identify, buffer_out);
        fpi_device_report_finger_status (device, FP_FINGER_STATUS_NEEDED);
        fp_info ("Sent identification request");
        break;
      }

    case IDENTIFY_GET_FINGER_INFO: {
        g_assert_nonnull (buffer_in);
        fpi_device_report_finger_status (device, FP_FINGER_STATUS_PRESENT);
        gboolean can_retry = FALSE;
        error = elanmoc2_get_finger_error (buffer_in, &can_retry);
        if (error != NULL)
          {
            fp_info ("Identify failed: %s", error->message);
            if (can_retry)
              {
                fp_info ("Retrying identification...");
                fpi_device_report_finger_status (device, FP_FINGER_STATUS_NONE);
                g_clear_error (&error);
                fpi_ssm_jump_to_state (ssm, IDENTIFY_IDENTIFY);
              }
            else
              {
                elanmoc2_identify_verify_complete (device,
                                                   g_steal_pointer (&error));
                fpi_ssm_mark_completed (g_steal_pointer (&self->ssm));
              }
            break;
          }

        g_assert_nonnull (data_in);
        g_assert (data_in_len >= 2);

        self->print_index = data_in[1];

        fp_info ("Identified finger %d; requesting finger info",
                 self->print_index);

        g_autoptr(GByteArray) buffer_out =
          elanmoc2_prepare_cmd (self, &cmd_finger_info);

        if (buffer_out == NULL)
          {
            fpi_ssm_next_state (ssm);
            break;
          }
        g_assert (buffer_out->len >= 4);
        buffer_out->data[3] = self->print_index;
        elanmoc2_cmd_transceive (device, &cmd_finger_info, buffer_out);
        break;
      }

    case IDENTIFY_CHECK_FINGER_INFO: {
        fpi_device_report_finger_status (device, FP_FINGER_STATUS_NONE);

        g_assert_nonnull (buffer_in);
        g_autoptr(FpPrint) print =
          elanmoc2_print_new_from_finger_info (self,
                                               self->print_index,
                                               buffer_in);

        error = NULL;
        elanmoc2_identify_verify_report (device,
                                         g_steal_pointer (&print),
                                         &error);
        elanmoc2_identify_verify_complete (device, g_steal_pointer (&error));
        fpi_ssm_mark_completed (g_steal_pointer (&self->ssm));
        break;
      }

    default:
      break;
    }
}

static void
elanmoc2_identify_verify (FpDevice *device)
{
  FpiDeviceElanMoC2 *self = FPI_DEVICE_ELANMOC2 (device);

  fp_info ("[elanmoc2] New identify/verify operation");
  self->ssm = fpi_ssm_new (device, elanmoc2_identify_run_state,
                           IDENTIFY_NUM_STATES);
  self->enrolled_num_retries = 0;
  fpi_ssm_start (self->ssm, elanmoc2_ssm_completed_callback);
}

static void
elanmoc2_enroll_ssm_completed_callback (FpiSsm *ssm, FpDevice *device,
                                        GError *error)
{
  FpiDeviceElanMoC2 *self = FPI_DEVICE_ELANMOC2 (device);

  /* Libfprint owns the print after completion. */
  self->enroll_print = NULL;
  elanmoc2_ssm_completed_callback (ssm, device, error);
}

static void
elanmoc2_enroll_run_state (FpiSsm *ssm, FpDevice *device)
{
  FpiDeviceElanMoC2 *self = FPI_DEVICE_ELANMOC2 (device);

  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) buffer_in = g_steal_pointer (&self->buffer_in);

  const guint8 *data_in =
    buffer_in != NULL ? g_bytes_get_data (buffer_in, NULL) : NULL;
  const gsize data_in_len =
    buffer_in != NULL ? g_bytes_get_size (buffer_in) : 0;

  g_assert_nonnull (self->enroll_print);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    /* First check how many fingers are already enrolled */
    case ENROLL_GET_NUM_ENROLLED: {
        elanmoc2_perform_get_num_enrolled (self, ssm);
        break;
      }

    case ENROLL_CHECK_NUM_ENROLLED: {
        if (data_in_len == 0)
          {
            error =
              elanmoc2_get_num_enrolled_retry_or_error (self,
                                                        ssm,
                                                        ENROLL_GET_NUM_ENROLLED);
            if (error != NULL)
              {
                fpi_device_enroll_complete (device,
                                            NULL,
                                            g_steal_pointer (&error));
                fpi_ssm_mark_completed (g_steal_pointer (&self->ssm));
              }
            break;
          }

        g_assert_nonnull (data_in);
        g_assert (data_in_len >= 2);
        /* Use the enrolled count as the next slot index (0-indexed).
         * If 0 enrolled, use slot 0. If 1 enrolled, use slot 1, etc. */
        self->enrolled_num = data_in[1];

        if (self->enrolled_num > ELANMOC2_MAX_PRINTS)
          {
            fp_info ("Can't enroll, sensor storage is full");
            error = fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_FULL,
                                              "Sensor storage is full");
            fpi_device_enroll_complete (device,
                                        NULL,
                                        g_steal_pointer (&error));
            fpi_ssm_mark_completed (g_steal_pointer (&self->ssm));
          }
        else
          {
            fp_info ("[elanmoc2] ARM-M4: assigned slot=%d, proceeding to PRIME_SENSOR",
                     self->enrolled_num);
            fpi_ssm_jump_to_state (ssm, ENROLL_PRIME_SENSOR);
          }
        break;
      }

    case ENROLL_EARLY_REENROLL_CHECK: {
        g_autoptr(GByteArray) buffer_out = elanmoc2_prepare_cmd (self,
                                                                 &cmd_identify);
        if (buffer_out == NULL)
          {
            fpi_ssm_next_state (ssm);
            break;
          }
        elanmoc2_cmd_transceive (device, &cmd_identify, buffer_out);
        fpi_device_report_finger_status (device, FP_FINGER_STATUS_NEEDED);
        fp_info ("Sent identification request");
        break;
      }

    case ENROLL_GET_ENROLLED_FINGER_INFO: {
        fpi_device_report_finger_status (device, FP_FINGER_STATUS_PRESENT);

        g_assert_nonnull (data_in);
        g_assert (g_bytes_get_size (buffer_in) >= 2);

        /* ARM-M4: The identify is used as a sensor priming step.
         * Regardless of the result (NOT_ENROLLED, NOT_ENOUGH_SURFACE, etc.),
         * always proceed to the enroll stage. The important thing is that
         * the identify command was sent. */
        fp_info ("[elanmoc2] ARM-M4: identify priming response: 0x%02x — proceeding to enroll",
                 data_in[1]);
        fpi_device_enroll_progress (device, self->enroll_stage, NULL, NULL);
        fpi_ssm_jump_to_state (ssm, ENROLL_ENROLL);
        break;
      }

    case ENROLL_ATTEMPT_DELETE: {
        fpi_device_report_finger_status (device, FP_FINGER_STATUS_NONE);
        fp_info ("Deleting enrolled finger %d", self->print_index);
        g_assert_nonnull (buffer_in);

        /* Attempt to delete the finger */
        g_autoptr(GBytes) user_id =
          elanmoc2_get_user_id_string (self, buffer_in);
        g_autoptr(GByteArray) buffer_out =
          elanmoc2_prepare_cmd (self, &cmd_delete);
        if (buffer_out == NULL)
          {
            fpi_ssm_next_state (ssm);
            break;
          }
        gsize user_id_bytes = MIN (cmd_delete.out_len - 4,
                                   ELANMOC2_USER_ID_MAX_LEN);
        g_assert (buffer_out->len >= 4 + user_id_bytes);
        buffer_out->data[3] = 0xf0 | (self->print_index + 5);
        memcpy (&buffer_out->data[4],
                g_bytes_get_data (user_id, NULL),
                user_id_bytes);
        elanmoc2_cmd_transceive (device, &cmd_delete, buffer_out);

        break;
      }

    case ENROLL_CHECK_DELETED: {
        g_assert_nonnull (data_in);
        g_assert (g_bytes_get_size (buffer_in) >= 2);

        if (data_in[1] != 0)
          {
            fp_info ("Failed to delete finger %d, wiping sensor",
                     self->print_index);
            fpi_ssm_jump_to_state (ssm, ENROLL_WIPE_SENSOR);
          }
        else
          {
            fp_info ("Finger %d deleted, proceeding with enroll stage",
                     self->print_index);
            self->enrolled_num--;
            fpi_device_enroll_progress (device, self->enroll_stage, NULL,
                                        NULL);
            fpi_ssm_jump_to_state (ssm, ENROLL_ENROLL);
          }
        break;
      }

    case ENROLL_WIPE_SENSOR: {
        g_autoptr(GByteArray) buffer_out =
          elanmoc2_prepare_cmd (self, &cmd_wipe_sensor);
        if (buffer_out == NULL)
          {
            fpi_ssm_next_state (ssm);
            break;
          }
        elanmoc2_cmd_transceive (device, &cmd_wipe_sensor, buffer_out);
        /* After wipe, first free slot is 0 (0-indexed) */
        self->enrolled_num = 0;
        self->print_index = 0;
        fp_info (
          "Wipe sensor command sent - next operation will take a while");
        /* cmd_wipe_sensor has in_len=0, callback auto-advances SSM */
        break;
      }

    case ENROLL_PRIME_SENSOR: {
        g_autoptr(GByteArray) buffer_out = elanmoc2_prepare_cmd (self,
                                                                 &cmd_identify);
        if (buffer_out == NULL)
          {
            fpi_ssm_next_state (ssm);
            break;
          }
        elanmoc2_cmd_transceive (device, &cmd_identify, buffer_out);
        fpi_device_report_finger_status (device, FP_FINGER_STATUS_NEEDED);
        fp_info ("Sent identification request (priming sensor)");
        break;
      }

    case ENROLL_PRIME_SENSOR_RESPONSE: {
        /* Priming response is not used. */
        if (data_in != NULL && data_in_len >= 2)
          fp_info ("[elanmoc2] ARM-M4: identify priming response: 0x%02x — proceeding to enroll",
                   data_in[1]);
        else
          fp_info ("[elanmoc2] ARM-M4: identify priming response: missing/short — proceeding to enroll");
        fpi_ssm_next_state (ssm);
        break;
      }

    case ENROLL_ENROLL: {
        g_autoptr(GByteArray) buffer_out = elanmoc2_prepare_cmd (self,
                                                                 &cmd_enroll);
        if (buffer_out == NULL)
          {
            fpi_ssm_next_state (ssm);
            break;
          }
        g_assert (buffer_out->len >= 7);
        buffer_out->data[3] = self->enrolled_num;
        buffer_out->data[4] = ELANMOC2_ENROLL_TIMES;
        buffer_out->data[5] = self->enroll_stage;
        buffer_out->data[6] = 0;
        elanmoc2_cmd_transceive (device, &cmd_enroll, buffer_out);
        fp_info ("Enroll command sent: %d/%d", self->enroll_stage,
                 ELANMOC2_ENROLL_TIMES);
        fpi_device_report_finger_status (device, FP_FINGER_STATUS_NEEDED);
        break;
      }

    case ENROLL_CHECK_ENROLLED: {
        fpi_device_report_finger_status (device, FP_FINGER_STATUS_PRESENT);

        g_assert_nonnull (data_in);
        g_assert (g_bytes_get_size (buffer_in) >= 2);

        if (data_in[1] == 0)
          {
            /* Stage okay */
            fp_info ("Enroll stage succeeded");
            self->enroll_stage++;
            fpi_device_enroll_progress (device, self->enroll_stage,
                                        self->enroll_print, NULL);
            if (self->enroll_stage >= ELANMOC2_ENROLL_TIMES)
              {
                fp_info ("Enroll completed");
                fpi_ssm_jump_to_state (ssm, ENROLL_LATE_REENROLL_CHECK);
                break;
              }
          }
        else
          {
            /* Detection error */
            gboolean can_retry = FALSE;
            error = elanmoc2_get_finger_error (buffer_in, &can_retry);
            if (error != NULL)
              {
                fp_info ("Enroll stage failed: %s", error->message);
                if (data_in[1] == ELANMOC2_RESP_NOT_ENROLLED)
                  {
                    /* Not enrolled is a fatal error for "identify" but not for
                     * "enroll" */
                    error->domain = FP_DEVICE_RETRY;
                    error->code = FP_DEVICE_RETRY_TOO_SHORT;
                    can_retry = FALSE;
                  }
            if (can_retry)
              {
                fpi_device_enroll_progress (device, self->enroll_stage, NULL,
                                            g_steal_pointer (&error));
              }
            else
              {
                fpi_device_enroll_complete (device, NULL,
                                            g_steal_pointer (&error));
                fpi_ssm_mark_completed (g_steal_pointer (&self->ssm));
                return;
              }
          }
        else
          {
            fp_info ("Enroll stage failed for unknown reasons");
          }
      }
    fp_info ("Performing another enroll");
    fpi_ssm_jump_to_state (ssm, ENROLL_ENROLL);
    break;
      }

    case ENROLL_RETRY_JUMP: {
        fpi_ssm_jump_to_state (ssm, ENROLL_ENROLL);
        break;
      }

    case ENROLL_LATE_REENROLL_CHECK: {
        fpi_device_report_finger_status (device, FP_FINGER_STATUS_NONE);
        g_autoptr(GByteArray) buffer_out =
          elanmoc2_prepare_cmd (self, &cmd_check_enroll_collision);
        if (buffer_out == NULL)
          {
            fpi_ssm_next_state (ssm);
            break;
          }
        elanmoc2_cmd_transceive (device, &cmd_check_enroll_collision, buffer_out);
        fp_info ("Check re-enroll command sent");
        break;
      }

    case ENROLL_PRE_COMMIT: {
        /* Collision check response; if clear, send slot marker 00 10 <slot>. */
        error = NULL;
        g_assert_nonnull (data_in);
        g_assert (data_in_len >= 2);

        if (data_in[1] != 0)
          {
            fp_info ("Finger already enrolled at position %d, cannot commit", data_in[2]);
            error = fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_DUPLICATE,
                                              "Finger is already enrolled");
            fpi_device_enroll_complete (device, NULL, g_steal_pointer (&error));
            fpi_ssm_mark_completed (g_steal_pointer (&self->ssm));
            self->enroll_print = NULL;
            break;
          }

        fp_info ("[elanmoc2] ARM-M4: no collision — sending pre-commit slot marker (00 10 %02x)",
                 self->enrolled_num);

        /* Build 3-byte buffer manually — NOT via elanmoc2_prepare_cmd (no 0x40 prefix) */
        g_autoptr(GByteArray) buffer_out = g_byte_array_new ();
        g_byte_array_set_size (buffer_out, 3);
        buffer_out->data[0] = 0x00;
        buffer_out->data[1] = 0x10;
        buffer_out->data[2] = (guchar) self->enrolled_num;
        elanmoc2_cmd_transceive (device, &cmd_arm_m4_commit_slot, buffer_out);
        /* Do NOT call fpi_ssm_next_state here — the send callback does it (in_len=0) */
        break;
      }

    case ENROLL_COMMIT: {
        /* No response from slot marker; send commit. */
        fp_info ("Sending commit command (slot 0x%02x)", self->enrolled_num);
        g_autoptr(GByteArray) buffer_out = elanmoc2_prepare_cmd (self,
                                                                 &cmd_commit);
        if (buffer_out == NULL)
          {
            fpi_ssm_next_state (ssm);
            break;
          }
        g_autofree gchar *user_id = fpi_print_generate_user_id (
          self->enroll_print);
        elanmoc2_print_set_data (self->enroll_print, self->enrolled_num,
                                 strlen (user_id), (guint8 *) user_id);

        g_assert (buffer_out->len == cmd_commit.out_len);
        buffer_out->data[3] = 0xf0 | (self->enrolled_num + 5);
        strncpy ((gchar *) &buffer_out->data[4], user_id, cmd_commit.out_len - 4);
        elanmoc2_cmd_transceive (device, &cmd_commit, buffer_out);
        fp_info ("Commit command sent");
        break;
      }

    case ENROLL_CHECK_COMMITTED: {
        error = NULL;

        g_assert_nonnull (data_in);
        g_assert (g_bytes_get_size (buffer_in) >= 2);

        if (data_in[1] != 0)
          {
            fp_info ("Commit failed with error code %d", data_in[1]);
            error = fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                              "Failed to store fingerprint for "
                                              "unknown reasons");
            fpi_device_enroll_complete (device, NULL, error);
            fpi_ssm_mark_failed (g_steal_pointer (&self->ssm),
                                 g_steal_pointer (&error));
          }
        else
          {
            fp_info ("Commit succeeded");
            fpi_device_enroll_complete (device,
                                        g_object_ref (self->enroll_print),
                                        NULL);
            fpi_ssm_mark_completed (g_steal_pointer (&self->ssm));
          }
        break;
      }
    }
}

static void
elanmoc2_enroll (FpDevice *device)
{
  FpiDeviceElanMoC2 *self = FPI_DEVICE_ELANMOC2 (device);

  fp_info ("[elanmoc2] New enroll operation");

  self->enroll_stage = 0;
  fpi_device_get_enroll_data (device, &self->enroll_print);

  self->ssm = fpi_ssm_new (device, elanmoc2_enroll_run_state,
                           ENROLL_NUM_STATES);
  self->enrolled_num_retries = 0;
  fpi_ssm_start (self->ssm, elanmoc2_enroll_ssm_completed_callback);
}

static void
elanmoc2_clear_storage_run_state (FpiSsm *ssm, FpDevice *device)
{
  FpiDeviceElanMoC2 *self = FPI_DEVICE_ELANMOC2 (device);

  g_autoptr(GByteArray) buffer_out = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) buffer_in = g_steal_pointer (&self->buffer_in);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case CLEAR_STORAGE_WIPE_SENSOR:
      buffer_out = elanmoc2_prepare_cmd (self, &cmd_wipe_sensor);
      if (buffer_out == NULL)
        {
          fpi_ssm_next_state (ssm);
          break;
        }
      elanmoc2_cmd_transceive (device, &cmd_wipe_sensor, buffer_out);
      fp_info ("Sent sensor wipe command, sensor will hang for ~5 seconds");
      break;

    case CLEAR_STORAGE_GET_NUM_ENROLLED:
      elanmoc2_perform_get_num_enrolled (self, ssm);
      break;

    case CLEAR_STORAGE_CHECK_NUM_ENROLLED: {
        gsize buffer_in_len = g_bytes_get_size (buffer_in);

        if (buffer_in_len == 0)
          {
            error =
              elanmoc2_get_num_enrolled_retry_or_error (self,
                                                        ssm,
                                                        CLEAR_STORAGE_GET_NUM_ENROLLED);
            if (error != NULL)
              {
                fpi_device_clear_storage_complete (device, g_steal_pointer (&error));
                fpi_ssm_mark_completed (g_steal_pointer (&self->ssm));
              }
            break;
          }


        /* It should take around 5 seconds to arrive here after the wipe sensor
         * command */
        g_assert_nonnull (buffer_in);
        g_assert (buffer_in_len >= 2);

        const guint8 *data_in = g_bytes_get_data (buffer_in, NULL);
        self->enrolled_num = data_in[1];

        if (self->enrolled_num == 0)
          {
            fpi_device_clear_storage_complete (device, NULL);
            fpi_ssm_mark_completed (g_steal_pointer (&self->ssm));
          }
        else
          {
            error = fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                              "Sensor erase requested but "
                                              "storage is not empty");
            fpi_device_clear_storage_complete (device, error);
            fpi_ssm_mark_failed (g_steal_pointer (&self->ssm),
                                 g_steal_pointer (&error));
            break;
          }
        break;
      }
    }
}

static void
elanmoc2_delete (FpDevice *device)
{
  fp_info ("[elanmoc2] Faking delete operation to satisfy fprintd garbage collection");
  fpi_device_delete_complete (device, NULL);
}

static void
elanmoc2_clear_storage_ssm_completed_callback (FpiSsm *ssm, FpDevice *device, GError *error)
{
  if (error)
    fpi_device_clear_storage_complete (device, error);
}

static void
elanmoc2_clear_storage (FpDevice *device)
{
  FpiDeviceElanMoC2 *self = FPI_DEVICE_ELANMOC2 (device);

  fp_info ("[elanmoc2] Starting clear storage operation");
  self->ssm = fpi_ssm_new (device, elanmoc2_clear_storage_run_state,
                           CLEAR_STORAGE_NUM_STATES);
  self->enrolled_num_retries = 0;
  fpi_ssm_start (self->ssm, elanmoc2_clear_storage_ssm_completed_callback);
}

static void
fpi_device_elanmoc2_init (FpiDeviceElanMoC2 *self)
{
}

static const FpIdEntry elanmoc2_id_table[] = {
  {.vid = ELANMOC2_VEND_ID, .pid = 0x0c6c, .driver_data = 0},
  {.vid = 0, .pid = 0, .driver_data = 0}
};

static void
fpi_device_elanmoc2_class_init (FpiDeviceElanMoC2Class *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);

  dev_class->id = FP_COMPONENT;
  dev_class->full_name = ELANMOC2_DRIVER_FULLNAME;

  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->id_table = elanmoc2_id_table;

  dev_class->nr_enroll_stages = ELANMOC2_ENROLL_TIMES;
  dev_class->temp_hot_seconds = -1;

  dev_class->open = elanmoc2_open;
  dev_class->close = elanmoc2_close;
  dev_class->identify = elanmoc2_identify_verify;
  dev_class->verify = elanmoc2_identify_verify;
  dev_class->enroll = elanmoc2_enroll;
  dev_class->delete = elanmoc2_delete;
  dev_class->clear_storage = elanmoc2_clear_storage;
  dev_class->cancel = elanmoc2_cancel;

  fpi_device_class_auto_initialize_features (dev_class);
  dev_class->features &= ~FP_DEVICE_FEATURE_DUPLICATES_CHECK;
  dev_class->features |= FP_DEVICE_FEATURE_UPDATE_PRINT;
}

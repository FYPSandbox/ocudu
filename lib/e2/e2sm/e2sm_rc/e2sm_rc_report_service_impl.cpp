/*
 *
 * Copyright 2021-2026 Software Radio Systems Limited
 *
 * This file is part of OCUDU.
 *
 * OCUDU is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * OCUDU is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#include "e2sm_rc_report_service_impl.h"
#include "ocudu/support/format/fmt_to_c_str.h"

using namespace asn1::e2ap;
using namespace asn1::e2sm;
using namespace ocudu;

e2sm_rc_report_service_style4::e2sm_rc_report_service_style4(e2sm_rc_action_definition_s action_def_,
                                                             odu::f1ap_ue_id_translator& f1ap_ue_id_provider_) :
  logger(ocudulog::fetch_basic_logger("E2SM-RC")),
  action_def(action_def_),
  f1ap_ue_id_provider(f1ap_ue_id_provider_)
{
  // Initialize indication header (Format 1 for event-based reporting)
  ric_ind_header.ric_ind_hdr_formats.set_ind_hdr_format1();

  // Initialize indication message (Format 2 for UE-level information)
  ric_ind_message.ric_ind_msg_formats.set_ind_msg_format2();

  logger.info("E2SM-RC Report Service Style 4 (UE Information) initialized");
}

e2sm_rc_report_service_style4::~e2sm_rc_report_service_style4()
{
  logger.info("E2SM-RC Report Service Style 4 destroyed");
}

void e2sm_rc_report_service_style4::on_ue_context_update(const e2_ue_context_info& ue_ctx)
{
  // Store in cache for measurement collection
  if (!ue_ctx.slices.empty()) {
    const auto& s_nssai = ue_ctx.slices.front();
    ue_context_cache[ue_ctx.ue_index] = std::make_pair(ue_ctx.crnti, s_nssai);

    // Track this UE as the one that triggered the event
    triggered_ue_index = ue_ctx.ue_index;
    is_disconnect_event = false;

    // Mark indication as ready and signal event for event-triggered reporting
    is_ind_msg_ready_ = true;
    report_event.set(); // Signal the event to wake up indication procedure

    logger.info("RC Report Style 4: UE context update, ue_index={}, crnti={:#x}, sst={}, sd={:#x}, cache_size={}",
                ue_ctx.ue_index,
                to_value(ue_ctx.crnti),
                s_nssai.sst.value(),
                s_nssai.sd.is_set() ? s_nssai.sd.value() : 0,
                ue_context_cache.size());
  }
}

void e2sm_rc_report_service_style4::on_ue_context_release(du_ue_index_t ue_index)
{
  auto it = ue_context_cache.find(ue_index);
  if (it != ue_context_cache.end()) {
    ue_context_cache.erase(it);

    // Mark as disconnect event (send empty indication)
    triggered_ue_index.reset(); // No specific UE to report
    is_disconnect_event = true;

    // Trigger indication to notify xApp of UE removal
    is_ind_msg_ready_ = true;
    report_event.set(); // Signal the event to wake up indication procedure

    logger.info("RC Report Style 4: UE context release, ue_index={}, cache_size={}", ue_index, ue_context_cache.size());
  }
}

bool e2sm_rc_report_service_style4::collect_measurements()
{
  // Clear previous indication message
  auto& msg_format2 = ric_ind_message.ric_ind_msg_formats.ind_msg_format2();
  msg_format2.ue_param_list.clear();

  // If disconnect event, send empty indication
  if (is_disconnect_event) {
    return true;
  }

  // If no specific UE triggered the event, skip
  if (!triggered_ue_index.has_value()) {
    return false;
  }

  // Only process the UE that triggered this event
  du_ue_index_t ue_index = triggered_ue_index.value();
  auto it = ue_context_cache.find(ue_index);

  if (it == ue_context_cache.end()) {
    return false;
  }

  // Build indication message for single UE
  {
    const auto& [ue_idx, ue_context] = *it;
    const rnti_t& rnti = ue_context.first;
    const s_nssai_t& s_nssai = ue_context.second;
    e2sm_rc_ind_msg_format2_item_s ue_param_item;
    ue_param_item.ext = false;

    // Get proper F1AP UE ID
    std::optional<gnb_cu_ue_f1ap_id_t> gnb_cu_ue_f1ap_id = f1ap_ue_id_provider.get_gnb_cu_ue_f1ap_id(ue_idx);
    if (!gnb_cu_ue_f1ap_id.has_value()) {
      logger.debug("RC Report Style 4: no F1AP ID for UE {}, skipping", ue_idx);
      return false;
    }

    // Set UE ID with F1AP ID
    ue_param_item.ue_id.set_gnb_du_ue_id();
    ue_param_item.ue_id.gnb_du_ue_id().gnb_cu_ue_f1ap_id = gnb_cu_ue_f1ap_id_to_uint(gnb_cu_ue_f1ap_id.value());
    ue_param_item.ue_id.gnb_du_ue_id().ran_ue_id_present = false;
    ue_param_item.ue_id.gnb_du_ue_id().cell_rnti.set_present();
    ue_param_item.ue_id.gnb_du_ue_id().cell_rnti->c_rnti = to_value(rnti);

    // Create RAN Parameter for S-NSSAI
    e2sm_rc_ind_msg_format2_ran_param_item_s ran_param;
    ran_param.ext = false;
    ran_param.ran_param_id = 1;

    // Encode S-NSSAI in RAN parameter value
    ran_param.ran_param_value_type.set_ran_p_choice_elem_false();
    auto& value = ran_param.ran_param_value_type.ran_p_choice_elem_false();
    value.ran_param_value_present = true;
    value.ran_param_value.set_value_oct_s();

    // Encode SST (1 byte) + SD (3 bytes if present)
    std::vector<uint8_t> s_nssai_bytes;
    s_nssai_bytes.push_back(s_nssai.sst.value());
    if (s_nssai.sd.is_set()) {
      uint32_t sd_val = s_nssai.sd.value();
      s_nssai_bytes.push_back((sd_val >> 16) & 0xFF);
      s_nssai_bytes.push_back((sd_val >> 8) & 0xFF);
      s_nssai_bytes.push_back(sd_val & 0xFF);
    }

    // Use from_bytes() to preserve raw byte values
    value.ran_param_value.value_oct_s().from_bytes(s_nssai_bytes);

    ue_param_item.ran_p_list.push_back(ran_param);
    msg_format2.ue_param_list.push_back(ue_param_item);

    logger.info("RC Report Style 4: collected ue_index={}, f1ap_id={}, crnti={:#x}, sst={}, sd={:#x}",
                ue_idx,
                gnb_cu_ue_f1ap_id_to_uint(gnb_cu_ue_f1ap_id.value()),
                to_value(rnti),
                s_nssai.sst.value(),
                s_nssai.sd.is_set() ? s_nssai.sd.value() : 0);
  }

  return true;
}

bool e2sm_rc_report_service_style4::is_ind_msg_ready()
{
  // Event-triggered: only ready when flag is set by UE context update
  return is_ind_msg_ready_;
}

ocudu::byte_buffer e2sm_rc_report_service_style4::get_indication_header()
{
  ocudu::byte_buffer buf;
  asn1::bit_ref       bref(buf);

  if (ric_ind_header.pack(bref) != asn1::OCUDUASN_SUCCESS) {
    logger.error("Failed to pack E2SM-RC indication header");
    return {};
  }

  return buf;
}

ocudu::byte_buffer e2sm_rc_report_service_style4::get_indication_message()
{
  ocudu::byte_buffer buf;
  asn1::bit_ref       bref(buf);

  if (ric_ind_message.pack(bref) != asn1::OCUDUASN_SUCCESS) {
    logger.error("Failed to pack E2SM-RC indication message");
    return {};
  }

  // Clear pending updates after successful packing
  clear_pending_indications();

  return buf;
}

void e2sm_rc_report_service_style4::clear_pending_indications()
{
  // Clear the event-triggered state after sending indication
  is_ind_msg_ready_ = false;
  triggered_ue_index.reset();
  is_disconnect_event = false;
}

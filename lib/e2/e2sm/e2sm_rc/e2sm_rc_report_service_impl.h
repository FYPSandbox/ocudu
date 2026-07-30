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

#pragma once

#include "ocudu/asn1/asn1_utils.h"
#include "ocudu/asn1/e2sm/e2sm_rc_ies.h"
#include "ocudu/e2/e2.h"
#include "ocudu/e2/e2sm/e2sm.h"
#include "ocudu/e2/e2_du_ue_context_notifier.h"
#include "ocudu/e2/e2_du_notifier_registry.h"
#include "ocudu/f1ap/du/f1ap_du.h"
#include <map>

namespace ocudu {

namespace odu {
class f1ap_ue_id_translator;
}

/// \brief E2SM-RC Report Service Style 4 - UE Information
///
/// This service reports UE context information including UE ID and associated S-NSSAI.
/// It is event-triggered and reports when:
/// - A new UE is added (UE Context Setup)
/// - UE context is modified (UE Context Modification)
/// - A UE is released (UE Context Release)
class e2sm_rc_report_service_style4 : public e2sm_report_service, public e2_du_ue_context_notifier
{
public:
  e2sm_rc_report_service_style4(asn1::e2sm::e2sm_rc_action_definition_s action_def_,
                                odu::f1ap_ue_id_translator&              f1ap_ue_id_provider_);
  virtual ~e2sm_rc_report_service_style4();

  /// e2sm_report_service functions.
  bool                collect_measurements() override;
  bool                is_ind_msg_ready() override;
  ocudu::byte_buffer  get_indication_message() override;
  ocudu::byte_buffer  get_indication_header() override;

  /// Event-triggered reporting support (E2SM-RC is event-driven)
  manual_event_flag& get_report_event_signal() override { return report_event; }
  bool supports_event_trigger() const override { return true; }

  /// e2_du_ue_context_notifier interface
  void on_ue_context_update(const e2_ue_context_info& ue_ctx) override;
  void on_ue_context_release(du_ue_index_t ue_index) override;

private:
  void clear_pending_indications();

  ocudulog::basic_logger&                  logger;
  asn1::e2sm::e2sm_rc_action_definition_s  action_def;
  odu::f1ap_ue_id_translator&              f1ap_ue_id_provider;
  asn1::e2sm::e2sm_rc_ind_hdr_s            ric_ind_header;
  asn1::e2sm::e2sm_rc_ind_msg_s            ric_ind_message;
  bool                                     is_ind_msg_ready_ = false;

  /// Event signal for event-triggered reporting
  manual_event_flag report_event;

  /// UE context cache for indication message generation
  /// Stores: UE Index -> (C-RNTI, S-NSSAI)
  std::map<du_ue_index_t, std::pair<rnti_t, s_nssai_t>> ue_context_cache;

  /// Track which UE triggered the current event (for single-UE indications)
  std::optional<du_ue_index_t> triggered_ue_index;
  bool is_disconnect_event = false;
};

} // namespace ocudu

// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/asn1/asn1_utils.h"
#include "ocudu/asn1/e2sm/e2sm_rc_ies.h"
#include "ocudu/e2/e2.h"
#include "ocudu/e2/e2_du_ue_context_notifier.h"
#include "ocudu/e2/e2sm/e2sm.h"
#include <map>
#include <mutex>
#include <vector>

namespace ocudu {

namespace odu {
class f1ap_ue_id_translator;
}

class e2sm_rc_report_service_style4;

class e2sm_rc_impl : public e2sm_interface, public e2_du_ue_context_notifier
{
public:
  e2sm_rc_impl(ocudulog::basic_logger& logger_, e2sm_handler& e2sm_packer_, odu::f1ap_ue_id_translator& f1ap_ue_id_provider_);
  ~e2sm_rc_impl() override;

  e2sm_handler& get_e2sm_packer() override;

  bool action_supported(const asn1::e2ap::ric_action_to_be_setup_item_s& ric_action) override;

  std::unique_ptr<e2sm_report_service> get_e2sm_report_service(const ocudu::byte_buffer& action_definition) override;
  e2sm_control_service*                get_e2sm_control_service(const e2sm_ric_control_request& request) override;

  bool add_e2sm_control_service(std::unique_ptr<e2sm_control_service> control_service) override;

  /// e2_du_ue_context_notifier interface; forwards UE context events to all active RC Report Style 4 instances.
  void on_ue_context_update(const e2_ue_context_info& ue_ctx) override;
  void on_ue_context_release(du_ue_index_t ue_index) override;

  void register_service(e2sm_rc_report_service_style4* service);
  void unregister_service(e2sm_rc_report_service_style4* service);

private:
  ocudulog::basic_logger&                                   logger;
  e2sm_handler&                                             e2sm_packer;
  odu::f1ap_ue_id_translator&                                f1ap_ue_id_provider;
  std::map<uint32_t, std::unique_ptr<e2sm_control_service>> control_services;

  std::mutex                                   report_services_mutex;
  std::vector<e2sm_rc_report_service_style4*>  active_report_services;
};
} // namespace ocudu

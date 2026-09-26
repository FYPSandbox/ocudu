// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "e2sm_rc_impl.h"
#include "e2sm_rc_control_service_impl.h"
#include "e2sm_rc_report_service_impl.h"
#include "ocudu/asn1/asn1_utils.h"
#include "ocudu/asn1/e2sm/e2sm_rc_ies.h"
#include "ocudu/e2/e2_du_notifier_registry.h"
#include "ocudu/e2/e2sm/e2sm.h"
#include <algorithm>

using namespace asn1::e2ap;
using namespace asn1::e2sm;
using namespace ocudu;

e2sm_rc_impl::e2sm_rc_impl(ocudulog::basic_logger&     logger_,
                           e2sm_handler&               e2sm_packer_,
                           odu::f1ap_ue_id_translator& f1ap_ue_id_provider_) :
  logger(logger_), e2sm_packer(e2sm_packer_), f1ap_ue_id_provider(f1ap_ue_id_provider_)
{
  // Register this instance with the global E2 DU notifier registry so the DU manager can forward
  // UE context setup/modification/release events to whichever RC Report Style 4 instances are active.
  e2_du_notifier_registry::get_instance().register_ue_context_notifier(this);
}

e2sm_rc_impl::~e2sm_rc_impl()
{
  e2_du_notifier_registry::get_instance().unregister_ue_context_notifier(this);
}

bool e2sm_rc_impl::action_supported(const ric_action_to_be_setup_item_s& ric_action)
{
  if (ric_action.ric_action_type.value != ric_action_type_e::report) {
    logger.debug("E2SM-RC: action type {} not supported", ric_action.ric_action_type.to_string());
    return false;
  }

  // Empty action definition -> default to Style 4 (matches how the RIC's Style 4 subscription is built).
  if (ric_action.ric_action_definition.size() == 0) {
    return true;
  }

  e2sm_rc_action_definition_s action_def;
  asn1::cbit_ref              bref(ric_action.ric_action_definition);
  if (action_def.unpack(bref) != asn1::OCUDUASN_SUCCESS) {
    logger.debug("E2SM-RC: failed to unpack action definition, defaulting to Style 4");
    return true;
  }

  if (action_def.ric_style_type != 4) {
    logger.debug("E2SM-RC: report style {} not supported (only Style 4 implemented)", action_def.ric_style_type);
    return false;
  }
  return true;
}

e2sm_handler& e2sm_rc_impl::get_e2sm_packer()
{
  return e2sm_packer;
}

namespace {
/// Auto-registers/unregisters an e2sm_rc_report_service_style4 instance with its owning e2sm_rc_impl for the
/// lifetime of the report service, so UE context events can be forwarded to it.
class e2sm_rc_report_service_style4_managed : public e2sm_rc_report_service_style4
{
public:
  e2sm_rc_report_service_style4_managed(e2sm_rc_action_definition_s action_def_,
                                        odu::f1ap_ue_id_translator& f1ap_ue_id_provider_,
                                        e2sm_rc_impl*               parent_) :
    e2sm_rc_report_service_style4(std::move(action_def_), f1ap_ue_id_provider_), parent(parent_)
  {
    parent->register_service(this);
  }

  ~e2sm_rc_report_service_style4_managed() override { parent->unregister_service(this); }

private:
  e2sm_rc_impl* parent;
};
} // namespace

std::unique_ptr<e2sm_report_service> e2sm_rc_impl::get_e2sm_report_service(const ocudu::byte_buffer& action_definition)
{
  e2sm_rc_action_definition_s default_action_def;
  default_action_def.ric_style_type = 4;

  return std::make_unique<e2sm_rc_report_service_style4_managed>(
      std::move(default_action_def), f1ap_ue_id_provider, this);
}

bool e2sm_rc_impl::add_e2sm_control_service(std::unique_ptr<e2sm_control_service> control_service)
{
  control_services.emplace(control_service->get_style_type(), std::move(control_service));
  return true;
}

e2sm_control_service* e2sm_rc_impl::get_e2sm_control_service(const e2sm_ric_control_request& request)
{
  const e2sm_rc_ctrl_hdr_s& ctrl_hdr = std::get<e2sm_rc_ctrl_hdr_s>(request.request_ctrl_hdr);

  int64_t ric_style_type = 0;
  if (ctrl_hdr.ric_ctrl_hdr_formats.type().value ==
      e2sm_rc_ctrl_hdr_s::ric_ctrl_hdr_formats_c_::types_opts::ctrl_hdr_format1) {
    e2sm_rc_ctrl_hdr_format1_s ctrl_hdr_format1 = ctrl_hdr.ric_ctrl_hdr_formats.ctrl_hdr_format1();
    ric_style_type                              = ctrl_hdr_format1.ric_style_type;
  } else if (ctrl_hdr.ric_ctrl_hdr_formats.type().value ==
             e2sm_rc_ctrl_hdr_s::ric_ctrl_hdr_formats_c_::types_opts::ctrl_hdr_format2) {
    ric_style_type = 255;
  } else if (ctrl_hdr.ric_ctrl_hdr_formats.type().value ==
             e2sm_rc_ctrl_hdr_s::ric_ctrl_hdr_formats_c_::types_opts::ctrl_hdr_format3) {
    e2sm_rc_ctrl_hdr_format3_s ctrl_hdr_format3 = ctrl_hdr.ric_ctrl_hdr_formats.ctrl_hdr_format3();
    ric_style_type                              = ctrl_hdr_format3.ric_style_type;
  } else {
    logger.error("RIC control header format not supported");
    return nullptr;
  }

  if (control_services.find(ric_style_type) != control_services.end()) {
    return control_services.at(ric_style_type).get();
  }
  return nullptr;
}

void e2sm_rc_impl::on_ue_context_update(const e2_ue_context_info& ue_ctx)
{
  std::lock_guard<std::mutex> lock(report_services_mutex);
  if (!ue_ctx.slices.empty()) {
    ue_context_cache[ue_ctx.ue_index] = ue_ctx;
  }
  for (auto* service : active_report_services) {
    if (service) {
      service->on_ue_context_update(ue_ctx);
    }
  }
}

void e2sm_rc_impl::on_ue_context_release(du_ue_index_t ue_index)
{
  std::lock_guard<std::mutex> lock(report_services_mutex);
  ue_context_cache.erase(ue_index);
  for (auto* service : active_report_services) {
    if (service) {
      service->on_ue_context_release(ue_index);
    }
  }
}

void e2sm_rc_impl::register_service(e2sm_rc_report_service_style4* service)
{
  std::lock_guard<std::mutex> lock(report_services_mutex);
  active_report_services.push_back(service);
  for (const auto& [ue_index, ue_ctx] : ue_context_cache) {
    service->on_ue_context_update(ue_ctx);
  }
}

void e2sm_rc_impl::unregister_service(e2sm_rc_report_service_style4* service)
{
  std::lock_guard<std::mutex> lock(report_services_mutex);
  auto it = std::find(active_report_services.begin(), active_report_services.end(), service);
  if (it != active_report_services.end()) {
    active_report_services.erase(it);
  }
}

// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "e2sm_ccc_asn1_packer.h"
#include "ocudu/asn1/e2sm/e2sm_ccc.h"

using namespace asn1::e2ap;
using namespace asn1::e2sm_ccc;
using namespace ocudu;

const std::string e2sm_ccc_asn1_packer::short_name       = "ORAN-E2SM-CCC";
const std::string e2sm_ccc_asn1_packer::oid              = "1.3.6.1.4.1.53148.1.6.2.4";
const std::string e2sm_ccc_asn1_packer::func_description = "Cell Configuration and Control";
const uint32_t    e2sm_ccc_asn1_packer::ran_func_id      = 4;
const uint32_t    e2sm_ccc_asn1_packer::revision         = 0;

static asn1::unbounded_octstring<true> json_to_octstring(const nlohmann::ordered_json& j)
{
  std::string          json_str = j.dump();
  std::vector<uint8_t> json_bytes(json_str.begin(), json_str.end());

  asn1::unbounded_octstring<true> octstr;
  if (not octstr.resize(json_bytes.size())) {
    printf("Failed to pack E2SM-CCC JSON message.\n");
    asn1::unbounded_octstring<true> empty;
    return empty;
  }
  std::copy(json_bytes.begin(), json_bytes.end(), octstr.begin());
  return octstr;
}

static expected<nlohmann::ordered_json, std::string> octstring_to_json(const asn1::unbounded_octstring<true>& octstr)
{
  std::string json_str(octstr.begin(), octstr.end());
  try {
    nlohmann::ordered_json j = nlohmann::ordered_json::parse(json_str);
    return j;
  } catch (const nlohmann::json::parse_error& e) {
    return make_unexpected(std::string("E2SM-CCC JSON parse error: ") + e.what());
  }
}

// The generated ASN.1 JSON converters are built without exceptions. Validate
// the supported slice-control profile before entering them, including types.
static bool valid_slice_control_json(const nlohmann::ordered_json& header, const nlohmann::ordered_json& message)
{
  using json = nlohmann::ordered_json;
  auto keys = [](const json& value, std::initializer_list<const char*> allowed) {
    if (not value.is_object()) return false;
    for (auto it = value.begin(); it != value.end(); ++it) {
      if (std::none_of(allowed.begin(), allowed.end(), [&](const char* key) { return it.key() == key; })) return false;
    }
    return true;
  };
  auto number = [](const json& value, const char* key, unsigned maximum) {
    return value.contains(key) and value[key].is_number_integer() and value[key] >= 0 and value[key] <= maximum;
  };
  auto digits = [](const json& value, const char* key, size_t minimum, size_t maximum, const char* alphabet) {
    if (not value.contains(key) or not value[key].is_string()) return false;
    const auto& text = value[key].get_ref<const std::string&>();
    return text.size() >= minimum and text.size() <= maximum and text.find_first_not_of(alphabet) == std::string::npos;
  };
  auto plmn = [&](const json& value) {
    return keys(value, {"mcc", "mnc"}) and digits(value, "mcc", 3, 3, "0123456789") and
           digits(value, "mnc", 2, 3, "0123456789");
  };
  if (not keys(header, {"controlHeaderFormat"}) or not header.contains("controlHeaderFormat")) return false;
  const auto& h = header["controlHeaderFormat"];
  if (not keys(h, {"ricStyleType"}) or not number(h, "ricStyleType", 2) or h["ricStyleType"] != 2) return false;
  if (not keys(message, {"controlMessageFormat"}) or not message.contains("controlMessageFormat")) return false;
  const auto& body = message["controlMessageFormat"];
  if (not keys(body, {"listOfCellsControlled"}) or not body.contains("listOfCellsControlled") or
      not body["listOfCellsControlled"].is_array() or body["listOfCellsControlled"].size() != 1) return false;
  for (const auto& cell : body["listOfCellsControlled"]) {
    if (not keys(cell, {"cellGlobalId", "listOfConfigurationStructures"}) or not cell.contains("cellGlobalId") or
        not cell.contains("listOfConfigurationStructures")) return false;
    const auto& cgi = cell["cellGlobalId"];
    if (not keys(cgi, {"plmnIdentity", "nRCellIdentity"}) or not cgi.contains("plmnIdentity") or
        not plmn(cgi["plmnIdentity"]) or not digits(cgi, "nRCellIdentity", 9, 9, "0123456789abcdefABCDEF")) return false;
    const auto& structures = cell["listOfConfigurationStructures"];
    if (not structures.is_array() or structures.empty() or structures.size() > 32) return false;
    for (const auto& structure : structures) {
      if (not keys(structure, {"ranConfigurationStructureName", "oldValuesOfAttributes", "newValuesOfAttributes"}) or
          not structure.contains("ranConfigurationStructureName") or structure["ranConfigurationStructureName"] != "O-RRMPolicyRatio") return false;
      for (const char* version : {"oldValuesOfAttributes", "newValuesOfAttributes"}) {
        if (not structure.contains(version) or not keys(structure[version], {"ranConfigurationStructure"}) or
            not structure[version].contains("ranConfigurationStructure")) return false;
        const auto& policy = structure[version]["ranConfigurationStructure"];
        if (not keys(policy, {"resourceType", "rRMPolicyMemberList", "rRMPolicyMinRatio", "rRMPolicyMaxRatio", "rRMPolicyDedicatedRatio"}) or
            not policy.contains("resourceType") or (policy["resourceType"] != "PRB_DL" and policy["resourceType"] != "PRB_UL") or
            not number(policy, "rRMPolicyMinRatio", 100) or not number(policy, "rRMPolicyMaxRatio", 100) or
            not number(policy, "rRMPolicyDedicatedRatio", 100) or not policy.contains("rRMPolicyMemberList")) return false;
        const auto& members = policy["rRMPolicyMemberList"];
        if (not members.is_array() or members.size() != 1) return false;
        const auto& member = members[0];
        if (not keys(member, {"plmnId", "snssai"}) or not member.contains("plmnId") or not plmn(member["plmnId"]) or
            not member.contains("snssai")) return false;
        const auto& slice = member["snssai"];
        if (not keys(slice, {"sst", "sd"}) or not number(slice, "sst", 255) or
            (slice.contains("sd") and not digits(slice, "sd", 6, 6, "0123456789abcdefABCDEF"))) return false;
      }
    }
  }
  return true;
}

e2sm_ccc_asn1_packer::e2sm_ccc_asn1_packer(std::vector<nr_cell_global_id_t> cells_) : cells(std::move(cells_)) {}

bool e2sm_ccc_asn1_packer::add_e2sm_control_service(e2sm_control_service* control_service)
{
  control_services.emplace(control_service->get_style_type(), control_service);
  return true;
}

e2sm_event_trigger_definition
e2sm_ccc_asn1_packer::handle_packed_event_trigger_definition(const ocudu::byte_buffer& event_trigger_definition)
{
  // TODO: add support for RIC subscriptions.
  printf("Failure - Trigger definition handling not supported in E2SM-CCC.\n");
  return {};
}

e2sm_action_definition
e2sm_ccc_asn1_packer::handle_packed_e2sm_action_definition(const ocudu::byte_buffer& action_definition)
{
  // TODO: add support for RIC subscriptions.
  printf("Failure - Action definition handling not supported in E2SM-CCC.\n");
  return {};
}

e2sm_ric_control_request
e2sm_ccc_asn1_packer::handle_packed_ric_control_request(const asn1::e2ap::ric_ctrl_request_s& req)
{
  e2sm_ric_control_request ric_control_request     = {};
  ric_control_request.service_model                = e2sm_service_model_t::CCC;
  ric_control_request.ric_call_process_id_present  = req->ric_call_process_id_present;
  ric_control_request.ric_ctrl_ack_request_present = req->ric_ctrl_ack_request_present;

  if (ric_control_request.ric_call_process_id_present) {
    ric_control_request.ric_call_process_id = req->ric_call_process_id.to_number();
  }
  // Keep CCC alternatives selected even on a malformed payload.
  ric_control_request.request_ctrl_hdr = ric_ctrl_hdr_s{};
  ric_control_request.request_ctrl_msg = ric_ctrl_msg_s{};
  try {
    auto header = octstring_to_json(req->ric_ctrl_hdr);
    auto message = octstring_to_json(req->ric_ctrl_msg);
    if (not header.has_value() or not message.has_value() or
        not valid_slice_control_json(header.value(), message.value())) {
      ric_control_request.decode_valid = false;
    } else {
      ric_control_request.request_ctrl_hdr = header.value().get<ric_ctrl_hdr_s>();
      ric_control_request.request_ctrl_msg = message.value().get<ric_ctrl_msg_s>();
    }
  } catch (const nlohmann::json::exception&) {
    ric_control_request.decode_valid = false;
  }

  if (ric_control_request.ric_ctrl_ack_request_present) {
    ric_control_request.ric_ctrl_ack_request =
        req->ric_ctrl_ack_request.value == asn1::e2ap::ric_ctrl_ack_request_e::ack;
  }
  return ric_control_request;
}

e2_ric_control_response e2sm_ccc_asn1_packer::pack_ric_control_response(const e2sm_ric_control_response& e2sm_response)
{
  e2_ric_control_response e2_control_response = {};
  e2_control_response.success                 = e2sm_response.success;

  if (e2_control_response.success) {
    if (e2sm_response.ric_ctrl_outcome_present) {
      e2_control_response.ack->ric_ctrl_outcome_present = true;
      nlohmann::ordered_json outcome_json       = std::get<ctrl_outcome_format_c>(e2sm_response.ric_ctrl_outcome);
      e2_control_response.ack->ric_ctrl_outcome = json_to_octstring(outcome_json);
    }
  } else {
    if (e2sm_response.ric_ctrl_outcome_present) {
      e2_control_response.failure->ric_ctrl_outcome_present = true;
      nlohmann::ordered_json outcome_json       = std::get<ctrl_outcome_format_c>(e2sm_response.ric_ctrl_outcome);
      e2_control_response.failure->ric_ctrl_outcome = json_to_octstring(outcome_json);
    }
    e2_control_response.failure->cause = e2sm_response.cause;
  }

  return e2_control_response;
}

asn1::unbounded_octstring<true> e2sm_ccc_asn1_packer::pack_ran_function_description()
{
  ran_function_definition_s ran_function_desc;

  // RAN Function name.
  ran_function_desc.ran_function_name.ran_function_short_name.resize(short_name.size());
  ran_function_desc.ran_function_name.ran_function_short_name.from_string(short_name);
  ran_function_desc.ran_function_name.ran_function_service_model_o_id.resize(oid.size());
  ran_function_desc.ran_function_name.ran_function_service_model_o_id.from_string(oid);
  ran_function_desc.ran_function_name.ran_function_description.resize(func_description.size());
  ran_function_desc.ran_function_name.ran_function_description.from_string(func_description);
  ran_function_desc.ran_function_name.ran_function_instance_present = true;
  ran_function_desc.ran_function_name.ran_function_instance         = revision;

  // E2 Node level config not supported.
  ran_function_desc.list_of_supported_node_level_cfg_structures.clear();

  // Cell-level configs.
  ran_function_desc.list_of_cells_for_ran_function_definition.resize(cells.size());
  for (size_t i = 0; i < cells.size(); ++i) {
  auto& cell_desc = ran_function_desc.list_of_cells_for_ran_function_definition[i];
  auto& nr_cgi = cell_desc.cell_global_id.set_nr_cgi();
  const auto identity = cells[i].plmn_id.to_string();
  nr_cgi.nr_cell_id.from_number(cells[i].nci.value());
  nr_cgi.plmn_id.mcc_present = true;
  nr_cgi.plmn_id.mcc.from_string(identity.substr(0, 3));
  nr_cgi.plmn_id.mnc_present = true;
  nr_cgi.plmn_id.mnc.from_string(identity.substr(3));

  // TODO: currently filled statically, it has to be taken from the loaded services.
  // Now only O-RRMPolicyRatio supported in Control service style 2 (cell-level).
  cell_desc.list_of_supported_cell_level_ran_cfg_structures.resize(1);
  auto& rrm_policy_ran_cfg_struct = cell_desc.list_of_supported_cell_level_ran_cfg_structures.back();
  rrm_policy_ran_cfg_struct.ran_cfg_structure_name.from_string("O-RRMPolicyRatio");
  rrm_policy_ran_cfg_struct.list_of_supported_attributes.resize(5);
  // O-RRMPolicyRatio::resourceType. Is-writable = False.
  attribute_s& resource_type_attribute = rrm_policy_ran_cfg_struct.list_of_supported_attributes[0];
  resource_type_attribute.attribute_name.from_string("resourceType");
  resource_type_attribute.supported_services.ctrl_service_present = true;
  resource_type_attribute.supported_services.ctrl_service.list_of_supported_ctrl_styles.resize(1);
  auto& resource_type_ctrl_style =
      resource_type_attribute.supported_services.ctrl_service.list_of_supported_ctrl_styles.back();
  resource_type_ctrl_style.ctrl_service_style_type = 2;
  resource_type_ctrl_style.ctrl_service_style_name.from_string("Cell Configuration and Control");
  resource_type_ctrl_style.ctrl_service_hdr_format_type            = 1;
  resource_type_ctrl_style.ctrl_service_msg_format_type            = 2;
  resource_type_ctrl_style.ric_call_process_id_format_type_present = false;
  resource_type_ctrl_style.ctrl_service_ctrl_outcome_format_type   = 2;
  // O-RRMPolicyRatio::rRMPolicyMemberList. Is-writable = True.
  attribute_s& member_list_attribute = rrm_policy_ran_cfg_struct.list_of_supported_attributes[1];
  member_list_attribute.attribute_name.from_string("rRMPolicyMemberList");
  member_list_attribute.supported_services.ctrl_service_present = true;
  member_list_attribute.supported_services.ctrl_service.list_of_supported_ctrl_styles.resize(1);
  auto& member_list_ctrl_style =
      member_list_attribute.supported_services.ctrl_service.list_of_supported_ctrl_styles.back();
  member_list_ctrl_style.ctrl_service_style_type = 2;
  member_list_ctrl_style.ctrl_service_style_name.from_string("Cell Configuration and Control");
  member_list_ctrl_style.ctrl_service_hdr_format_type            = 1;
  member_list_ctrl_style.ctrl_service_msg_format_type            = 2;
  member_list_ctrl_style.ric_call_process_id_format_type_present = false;
  member_list_ctrl_style.ctrl_service_ctrl_outcome_format_type   = 2;
  // O-RRMPolicyRatio::rRMPolicyMaxRatio. Is-writable = True.
  attribute_s& policy_max_attribute = rrm_policy_ran_cfg_struct.list_of_supported_attributes[2];
  policy_max_attribute.attribute_name.from_string("rRMPolicyMaxRatio");
  policy_max_attribute.supported_services.ctrl_service_present = true;
  policy_max_attribute.supported_services.ctrl_service.list_of_supported_ctrl_styles.resize(1);
  auto& policy_max_ctrl_style =
      policy_max_attribute.supported_services.ctrl_service.list_of_supported_ctrl_styles.back();
  policy_max_ctrl_style.ctrl_service_style_type = 2;
  policy_max_ctrl_style.ctrl_service_style_name.from_string("Cell Configuration and Control");
  policy_max_ctrl_style.ctrl_service_hdr_format_type            = 1;
  policy_max_ctrl_style.ctrl_service_msg_format_type            = 2;
  policy_max_ctrl_style.ric_call_process_id_format_type_present = false;
  policy_max_ctrl_style.ctrl_service_ctrl_outcome_format_type   = 2;
  // O-RRMPolicyRatio::rRMPolicyMinRatio. Is-writable = True.
  attribute_s& policy_min_attribute = rrm_policy_ran_cfg_struct.list_of_supported_attributes[3];
  policy_min_attribute.attribute_name.from_string("rRMPolicyMinRatio");
  policy_min_attribute.supported_services.ctrl_service_present = true;
  policy_min_attribute.supported_services.ctrl_service.list_of_supported_ctrl_styles.resize(1);
  auto& policy_min_ctrl_style =
      policy_min_attribute.supported_services.ctrl_service.list_of_supported_ctrl_styles.back();
  policy_min_ctrl_style.ctrl_service_style_type = 2;
  policy_min_ctrl_style.ctrl_service_style_name.from_string("Cell Configuration and Control");
  policy_min_ctrl_style.ctrl_service_hdr_format_type            = 1;
  policy_min_ctrl_style.ctrl_service_msg_format_type            = 2;
  policy_min_ctrl_style.ric_call_process_id_format_type_present = false;
  policy_min_ctrl_style.ctrl_service_ctrl_outcome_format_type   = 2;
  // O-RRMPolicyRatio::rRMPolicyDedicatedRatio. Is-writable = True.
  attribute_s& policy_ded_attribute = rrm_policy_ran_cfg_struct.list_of_supported_attributes[4];
  policy_ded_attribute.attribute_name.from_string("rRMPolicyDedicatedRatio");
  policy_ded_attribute.supported_services.ctrl_service_present = true;
  policy_ded_attribute.supported_services.ctrl_service.list_of_supported_ctrl_styles.resize(1);
  auto& policy_ded_ctrl_style =
      policy_ded_attribute.supported_services.ctrl_service.list_of_supported_ctrl_styles.back();
  policy_ded_ctrl_style.ctrl_service_style_type = 2;
  policy_ded_ctrl_style.ctrl_service_style_name.from_string("Cell Configuration and Control");
  policy_ded_ctrl_style.ctrl_service_hdr_format_type            = 1;
  policy_ded_ctrl_style.ctrl_service_msg_format_type            = 2;
  policy_ded_ctrl_style.ric_call_process_id_format_type_present = false;
  policy_ded_ctrl_style.ctrl_service_ctrl_outcome_format_type   = 2;

  }
  nlohmann::ordered_json json = ran_function_desc;
  return json_to_octstring(json);
}

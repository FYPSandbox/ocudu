// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
#include "common/e2_test_helpers.h"
#include "lib/e2/e2sm/e2sm_ccc/e2sm_ccc_asn1_packer.h"
#include "lib/e2/e2sm/e2sm_ccc/e2sm_ccc_control_action_du_executor.h"
#include "ocudu/support/executors/manual_task_worker.h"
#include <fstream>
#include <gtest/gtest.h>

using namespace ocudu;
using namespace asn1::e2sm_ccc;

class ccc_control_test : public ::testing::Test
{
protected:
  dummy_du_configurator du;
  manual_task_worker worker{64};
  e2sm_ccc_control_o_rrm_policy_ratio_executor executor{du, worker};
  e2sm_ccc_asn1_packer packer;
  e2sm_ric_control_request req;
  void SetUp() override
  {
    const char* fixture = std::getenv("FYP_CCC_FIXTURE");
    ASSERT_NE(fixture, nullptr) << "Set FYP_CCC_FIXTURE to the Python-generated E2AP request.bin";
    std::ifstream stream(fixture, std::ios::binary);
    ASSERT_TRUE(stream.good());
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(stream), {}};
    auto buffer = byte_buffer::create(bytes);
    ASSERT_TRUE(buffer.has_value());
    asn1::cbit_ref ref(*buffer);
    asn1::e2ap::e2ap_pdu_c pdu;
    ASSERT_EQ(pdu.unpack(ref), asn1::OCUDUASN_SUCCESS);
    auto& request = pdu.init_msg().value.ric_ctrl_request();
    ASSERT_EQ(request->ran_function_id, 4);
    req = packer.handle_packed_ric_control_request(request);
    ASSERT_TRUE(req.decode_valid);
  }
  auto& structures() { return std::get<ric_ctrl_msg_s>(req.request_ctrl_msg).ctrl_msg_format.ctrl_msg_format2().list_of_cells_ctrl[0].list_of_cfg_structures; }
};

TEST_F(ccc_control_test, python_e2ap_and_json_decode_and_zero_dedicated_are_supported)
{
  ASSERT_TRUE(executor.ric_control_action_supported(req));
  ASSERT_EQ(structures().size(), 4);
}
TEST_F(ccc_control_test, unpaired_direction_is_rejected)
{
  structures().resize(3);
  ASSERT_FALSE(executor.ric_control_action_supported(req));
}
TEST_F(ccc_control_test, inconsistent_direction_limits_are_rejected)
{
  structures()[1].new_values_of_attributes.ran_cfg_structure.o_rrm_policy_ratio().rrm_policy_max_ratio = 51;
  ASSERT_FALSE(executor.ric_control_action_supported(req));
}
TEST_F(ccc_control_test, negative_and_missing_ratios_are_rejected)
{
  auto& p = structures()[0].new_values_of_attributes.ran_cfg_structure.o_rrm_policy_ratio();
  p.rrm_policy_ded_ratio = -1;
  ASSERT_FALSE(executor.ric_control_action_supported(req));
  p.rrm_policy_ded_ratio = 0;
  p.rrm_policy_ded_ratio_present = false;
  ASSERT_FALSE(executor.ric_control_action_supported(req));
}
TEST_F(ccc_control_test, invalid_ratio_order_is_rejected)
{
  structures()[0].new_values_of_attributes.ran_cfg_structure.o_rrm_policy_ratio().rrm_policy_min_ratio = 90;
  ASSERT_FALSE(executor.ric_control_action_supported(req));
}
TEST_F(ccc_control_test, absent_sd_and_zero_sd_are_distinct)
{
  structures()[1].new_values_of_attributes.ran_cfg_structure.o_rrm_policy_ratio().rrm_policy_member_list[0].snssai.sd_present = false;
  ASSERT_FALSE(executor.ric_control_action_supported(req));
}
TEST_F(ccc_control_test, empty_and_multicell_requests_are_rejected)
{
  auto& cells = std::get<ric_ctrl_msg_s>(req.request_ctrl_msg).ctrl_msg_format.ctrl_msg_format2().list_of_cells_ctrl;
  cells.resize(2);
  ASSERT_FALSE(executor.ric_control_action_supported(req));
  cells.resize(0);
  ASSERT_FALSE(executor.ric_control_action_supported(req));
}
TEST_F(ccc_control_test, failure_outcome_uses_failure_container)
{
  e2sm_ric_control_response response{};
  response.success = false;
  response.ric_ctrl_outcome_present = true;
  response.cause.set_misc().value = asn1::e2ap::cause_misc_e::options::unspecified;
  ctrl_outcome_format_c outcome;
  outcome.set_ctrl_outcome_format2();
  response.ric_ctrl_outcome = outcome;
  auto packed = packer.pack_ric_control_response(response);
  ASSERT_FALSE(packed.success);
  ASSERT_TRUE(packed.failure->ric_ctrl_outcome_present);
  ASSERT_GT(packed.failure->ric_ctrl_outcome.size(), 0);
}

#include "lib/e2/e2sm/e2sm_kpm/e2sm_kpm_du_meas_provider_impl.h"

TEST(ccc_capabilities, advertises_actual_cell_identity)
{
  nr_cell_global_id_t cell{plmn_identity::parse("00101").value(), nr_cell_identity::create(6750208).value()};
  e2sm_ccc_asn1_packer packer({cell});
  auto encoded = packer.pack_ran_function_description();
  auto json = nlohmann::ordered_json::parse(encoded.begin(), encoded.end());
  ran_function_definition_s decoded = json;
  ASSERT_EQ(decoded.list_of_cells_for_ran_function_definition.size(), 1);
  ASSERT_EQ(decoded.list_of_cells_for_ran_function_definition[0].cell_global_id.nr_cgi().nr_cell_id.to_number(), 6750208);
}

TEST(ccc_kpm, uses_scheduler_slice_counters_and_exact_identity)
{
  dummy_f1ap_ue_id_translator translator;
  e2sm_kpm_du_meas_provider_impl provider(translator);
  scheduler_cell_metrics metrics{};
  metrics.nof_prbs = 100;
  metrics.nof_dl_slots = 10;
  metrics.nof_ul_slots = 10;
  scheduler_slice_metrics slice;
  slice.member.s_nssai = s_nssai_t{slice_service_type{1}};
  slice.dl_prbs = 150;
  metrics.slice_metrics.push_back(slice);
  provider.report_metrics(metrics);
  asn1::e2sm::meas_type_c type;
  type.set_meas_name().from_string("RRU.PrbUsedDl");
  asn1::e2sm::label_info_list_l labels;
  labels.resize(1);
  auto& label = labels[0].meas_label;
  label.slice_id_present = true;
  label.slice_id.sst.from_number(1);
  std::vector<asn1::e2sm::meas_record_item_c> items;
  ASSERT_TRUE(provider.get_meas_data(type, labels, {}, {}, items));
  ASSERT_EQ(items[0].type(), asn1::e2sm::meas_record_item_c::types::integer);
  ASSERT_EQ(items[0].integer(), 15);
  label.slice_id.sd_present = true;
  label.slice_id.sd.from_number(0);
  items.clear();
  ASSERT_TRUE(provider.get_meas_data(type, labels, {}, {}, items));
  ASSERT_EQ(items[0].type(), asn1::e2sm::meas_record_item_c::types::no_value);
  label.slice_id.sd_present = false;
  // Two PLMNs with the same S-NSSAI cannot be distinguished by a SliceID-only label.
  slice.member.plmn_id = plmn_identity::parse("00202").value();
  metrics.slice_metrics.push_back(slice);
  provider.report_metrics(metrics);
  items.clear();
  ASSERT_TRUE(provider.get_meas_data(type, labels, {}, {}, items));
  ASSERT_EQ(items[0].type(), asn1::e2sm::meas_record_item_c::types::no_value);
}

TEST(ccc_context_fixture, encode_actual_rc_membership_message)
{
  const char* output = std::getenv("FYP_RC_CONTEXT_FIXTURE");
  ASSERT_NE(output, nullptr);
  asn1::e2sm::e2sm_rc_ind_msg_s message;
  auto& body = message.ric_ind_msg_formats.set_ind_msg_format2();
  asn1::e2sm::e2sm_rc_ind_msg_format2_item_s item;
  auto& ue = item.ue_id.set_gnb_du_ue_id();
  ue.gnb_cu_ue_f1ap_id = 513;
  ue.cell_rnti.set_present();
  ue.cell_rnti->c_rnti = 0x4601;
  asn1::e2sm::e2sm_rc_ind_msg_format2_ran_param_item_s parameter;
  parameter.ran_param_id = 1;
  auto& value = parameter.ran_param_value_type.set_ran_p_choice_elem_false();
  value.ran_param_value_present = true;
  value.ran_param_value.set_value_oct_s().from_bytes(std::vector<uint8_t>{2,0,0,0});
  item.ran_p_list.push_back(parameter);
  body.ue_param_list.push_back(item);
  byte_buffer buffer;
  asn1::bit_ref ref(buffer);
  ASSERT_EQ(message.pack(ref), asn1::OCUDUASN_SUCCESS);
  std::ofstream stream(output, std::ios::binary);
  for (uint8_t b : buffer) stream.put(static_cast<char>(b));
  ASSERT_TRUE(stream.good());
}

TEST_F(ccc_control_test, missing_json_fields_are_rejected_without_crashing)
{
  asn1::e2ap::ric_ctrl_request_s wire;
  const std::string empty = "{}";
  ASSERT_TRUE(wire->ric_ctrl_hdr.resize(empty.size()));
  ASSERT_TRUE(wire->ric_ctrl_msg.resize(empty.size()));
  std::copy(empty.begin(), empty.end(), wire->ric_ctrl_hdr.begin());
  std::copy(empty.begin(), empty.end(), wire->ric_ctrl_msg.begin());
  ASSERT_FALSE(packer.handle_packed_ric_control_request(wire).decode_valid);
}

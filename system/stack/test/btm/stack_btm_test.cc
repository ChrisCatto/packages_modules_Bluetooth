/*
 *
 *  Copyright 2020 The Android Open Source Project
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <vector>

#include "btif/include/btif_hh.h"
#include "common/init_flags.h"
#include "hci/include/hci_layer.h"
#include "internal_include/stack_config.h"
#include "osi/include/allocator.h"
#include "osi/include/osi.h"
#include "stack/btm/btm_dev.h"
#include "stack/btm/btm_int_types.h"
#include "stack/btm/btm_sco.h"
#include "stack/btm/btm_sec.h"
#include "stack/btm/security_device_record.h"
#include "stack/include/acl_api.h"
#include "stack/include/acl_hci_link_interface.h"
#include "stack/include/btm_client_interface.h"
#include "stack/include/hcidefs.h"
#include "stack/include/sec_hci_link_interface.h"
#include "stack/l2cap/l2c_int.h"
#include "test/common/mock_functions.h"
#include "test/mock/mock_device_iot_config.h"
#include "test/mock/mock_osi_list.h"
#include "test/mock/mock_stack_hcic_hcicmds.h"
#include "types/raw_address.h"

using testing::Each;
using testing::Eq;

namespace mock = test::mock::stack_hcic_hcicmds;

extern tBTM_CB btm_cb;

uint8_t btif_trace_level = BT_TRACE_LEVEL_DEBUG;
uint8_t appl_trace_level = BT_TRACE_LEVEL_VERBOSE;
btif_hh_cb_t btif_hh_cb;
tL2C_CB l2cb;

const hci_t* hci_layer_get_interface() { return nullptr; }

const std::string kSmpOptions("mock smp options");
const std::string kBroadcastAudioConfigOptions(
    "mock broadcast audio config options");

namespace {

using testing::_;
using testing::DoAll;
using testing::NotNull;
using testing::Pointee;
using testing::Return;
using testing::SaveArg;
using testing::SaveArgPointee;
using testing::StrEq;
using testing::StrictMock;
using testing::Test;

// NOTE: The production code allows N+1 device records.
constexpr size_t kBtmSecMaxDeviceRecords =
    static_cast<size_t>(BTM_SEC_MAX_DEVICE_RECORDS + 1);

std::string Hex16(int n) {
  std::ostringstream oss;
  oss << "0x" << std::hex << std::setw(4) << std::setfill('0') << n;
  return oss.str();
}

class StackBtmTest : public Test {
 public:
 protected:
  void SetUp() override { reset_mock_function_count_map(); }
  void TearDown() override {}
};

class StackBtmWithInitFreeTest : public StackBtmTest {
 public:
 protected:
  void SetUp() override {
    StackBtmTest::SetUp();
    btm_cb.Init(BTM_SEC_MODE_SC);
  }
  void TearDown() override {
    btm_cb.Free();
    StackBtmTest::TearDown();
  }
};

TEST_F(StackBtmTest, GlobalLifecycle) {
  get_btm_client_interface().lifecycle.btm_init();
  get_btm_client_interface().lifecycle.btm_free();
}

TEST_F(StackBtmTest, DynamicLifecycle) {
  auto* btm = new tBTM_CB();
  delete btm;
}

TEST_F(StackBtmTest, InitFree) {
  btm_cb.Init(0x1);
  btm_cb.Free();
}

TEST_F(StackBtmTest, tSCO_CB) {
  bluetooth::common::InitFlags::SetAllForTesting();
  tSCO_CB* p_sco = &btm_cb.sco_cb;
  p_sco->Init();
  p_sco->Free();
}

TEST_F(StackBtmTest, InformClientOnConnectionSuccess) {
  get_btm_client_interface().lifecycle.btm_init();

  RawAddress bda({0x11, 0x22, 0x33, 0x44, 0x55, 0x66});

  btm_acl_connected(bda, 2, HCI_SUCCESS, false);
  ASSERT_EQ(1, get_func_call_count("BTA_dm_acl_up"));

  get_btm_client_interface().lifecycle.btm_free();
}

TEST_F(StackBtmTest, NoInformClientOnConnectionFail) {
  get_btm_client_interface().lifecycle.btm_init();

  RawAddress bda({0x11, 0x22, 0x33, 0x44, 0x55, 0x66});

  btm_acl_connected(bda, 2, HCI_ERR_NO_CONNECTION, false);
  ASSERT_EQ(0, get_func_call_count("BTA_dm_acl_up"));

  get_btm_client_interface().lifecycle.btm_free();
}

TEST_F(StackBtmTest, default_packet_type) {
  get_btm_client_interface().lifecycle.btm_init();

  btm_cb.acl_cb_.SetDefaultPacketTypeMask(0x4321);
  ASSERT_EQ(0x4321, btm_cb.acl_cb_.DefaultPacketTypes());

  get_btm_client_interface().lifecycle.btm_free();
}

TEST_F(StackBtmTest, change_packet_type) {
  int cnt = 0;
  get_btm_client_interface().lifecycle.btm_init();

  btm_cb.acl_cb_.SetDefaultPacketTypeMask(0xffff);
  ASSERT_EQ(0xffff, btm_cb.acl_cb_.DefaultPacketTypes());

  // Create connection
  RawAddress bda({0x11, 0x22, 0x33, 0x44, 0x55, 0x66});
  btm_acl_created(bda, 0x123, HCI_ROLE_CENTRAL, BT_TRANSPORT_BR_EDR);

  uint64_t features = 0xffffffffffffffff;
  acl_process_supported_features(0x123, features);

  uint16_t handle{0};
  uint16_t packet_types{0};

  mock::btsnd_hcic_change_conn_type.body = [&handle, &packet_types](
                                               uint16_t h, uint16_t p) {
    handle = h;
    packet_types = p;
  };
  btm_set_packet_types_from_address(bda, 0x55aa);
  ASSERT_EQ(++cnt, get_func_call_count("btsnd_hcic_change_conn_type"));
  ASSERT_EQ(0x123, handle);
  ASSERT_EQ(Hex16(0x4400 | HCI_PKT_TYPES_MASK_DM1), Hex16(packet_types));

  btm_set_packet_types_from_address(bda, 0xffff);
  ASSERT_EQ(++cnt, get_func_call_count("btsnd_hcic_change_conn_type"));
  ASSERT_EQ(0x123, handle);
  ASSERT_EQ(Hex16(0xcc00 | HCI_PKT_TYPES_MASK_DM1 | HCI_PKT_TYPES_MASK_DH1),
            Hex16(packet_types));

  btm_set_packet_types_from_address(bda, 0x0);
  ASSERT_EQ(0x123, handle);
  ASSERT_EQ(Hex16(0xcc18), Hex16(packet_types));

  mock::btsnd_hcic_change_conn_type = {};
  get_btm_client_interface().lifecycle.btm_free();
}

TEST(ScoTest, make_sco_packet) {
  std::vector<uint8_t> data = {10, 20, 30};
  uint16_t handle = 0xab;
  BT_HDR* p = btm_sco_make_packet(data, handle);
  ASSERT_EQ(p->event, BT_EVT_TO_LM_HCI_SCO);
  ASSERT_EQ(p->len, 3 + data.size());
  ASSERT_EQ(p->data[0], 0xab);
  ASSERT_EQ(p->data[1], 0);
  ASSERT_EQ(p->data[2], 3);
  ASSERT_EQ(p->data[3], 10);
  ASSERT_EQ(p->data[4], 20);
  ASSERT_EQ(p->data[5], 30);
  osi_free(p);
}

TEST(BtmTest, BTM_EIR_MAX_SERVICES) { ASSERT_EQ(46, BTM_EIR_MAX_SERVICES); }

}  // namespace

void btm_sec_rmt_name_request_complete(const RawAddress* p_bd_addr,
                                       const uint8_t* p_bd_name,
                                       tHCI_STATUS status);

struct {
  RawAddress bd_addr;
  DEV_CLASS dc;
  tBTM_BD_NAME bd_name;
} btm_test;

TEST(SecTest, btm_sec_rmt_name_request_complete) {
  bluetooth::common::InitFlags::SetAllForTesting();
  btm_cb.Init(0);

  ASSERT_TRUE(BTM_SecAddRmtNameNotifyCallback(
      [](const RawAddress& bd_addr, DEV_CLASS dc, tBTM_BD_NAME bd_name) {
        btm_test.bd_addr = bd_addr;
        memcpy(btm_test.dc, dc, DEV_CLASS_LEN);
        memcpy(btm_test.bd_name, bd_name, BTM_MAX_REM_BD_NAME_LEN);
      }));

  RawAddress bd_addr = RawAddress({0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6});
  const uint8_t* p_bd_name = (const uint8_t*)"MyTestName";

  btm_test = {};
  btm_sec_rmt_name_request_complete(&bd_addr, p_bd_name, HCI_SUCCESS);

  ASSERT_THAT(btm_test.bd_name, Each(Eq(0)));
  ASSERT_THAT(btm_test.dc, Each(Eq(0)));
  ASSERT_EQ(bd_addr, btm_test.bd_addr);

  btm_test = {};
  ASSERT_TRUE(btm_find_or_alloc_dev(bd_addr) != nullptr);
  btm_sec_rmt_name_request_complete(&bd_addr, p_bd_name, HCI_SUCCESS);

  ASSERT_STREQ((const char*)p_bd_name, (const char*)btm_test.bd_name);
  ASSERT_THAT(btm_test.dc, Each(Eq(0)));
  ASSERT_EQ(bd_addr, btm_test.bd_addr);

  btm_cb.Free();
}

TEST_F(StackBtmWithInitFreeTest, btm_sec_encrypt_change) {
  bluetooth::common::InitFlags::SetAllForTesting();

  RawAddress bd_addr = RawAddress({0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6});
  const uint16_t classic_handle = 0x1234;
  const uint16_t ble_handle = 0x9876;

  // Check the collision conditionals
  btm_cb.collision_start_time = 0UL;
  btm_sec_encrypt_change(classic_handle, HCI_ERR_LMP_ERR_TRANS_COLLISION, 0x01);
  uint64_t collision_start_time = btm_cb.collision_start_time;
  ASSERT_NE(0UL, collision_start_time);

  btm_cb.collision_start_time = 0UL;
  btm_sec_encrypt_change(classic_handle, HCI_ERR_DIFF_TRANSACTION_COLLISION,
                         0x01);
  collision_start_time = btm_cb.collision_start_time;
  ASSERT_NE(0UL, collision_start_time);

  // No device
  btm_cb.collision_start_time = 0;
  btm_sec_encrypt_change(classic_handle, HCI_SUCCESS, 0x01);
  ASSERT_EQ(0UL, btm_cb.collision_start_time);

  // Setup device
  tBTM_SEC_DEV_REC* device_record = btm_sec_allocate_dev_rec();
  ASSERT_NE(nullptr, device_record);
  ASSERT_EQ(BTM_SEC_IN_USE, device_record->sec_flags);
  device_record->bd_addr = bd_addr;
  device_record->hci_handle = classic_handle;
  device_record->ble_hci_handle = ble_handle;

  // With classic device encryption enable
  btm_sec_encrypt_change(classic_handle, HCI_SUCCESS, 0x01);
  ASSERT_EQ(BTM_SEC_IN_USE | BTM_SEC_AUTHENTICATED | BTM_SEC_ENCRYPTED,
            device_record->sec_flags);

  // With classic device encryption disable
  btm_sec_encrypt_change(classic_handle, HCI_SUCCESS, 0x00);
  ASSERT_EQ(BTM_SEC_IN_USE | BTM_SEC_AUTHENTICATED, device_record->sec_flags);
  device_record->sec_flags = BTM_SEC_IN_USE;

  // With le device encryption enable
  btm_sec_encrypt_change(ble_handle, HCI_SUCCESS, 0x01);
  ASSERT_EQ(BTM_SEC_IN_USE | BTM_SEC_LE_ENCRYPTED, device_record->sec_flags);

  // With le device encryption disable
  btm_sec_encrypt_change(ble_handle, HCI_SUCCESS, 0x00);
  ASSERT_EQ(BTM_SEC_IN_USE, device_record->sec_flags);
  device_record->sec_flags = BTM_SEC_IN_USE;

  wipe_secrets_and_remove(device_record);
}

TEST_F(StackBtmWithInitFreeTest, BTM_SetEncryption) {
  const RawAddress bd_addr = RawAddress({0x11, 0x22, 0x33, 0x44, 0x55, 0x66});
  const tBT_TRANSPORT transport{BT_TRANSPORT_LE};
  tBTM_SEC_CALLBACK* p_callback{nullptr};
  tBTM_BLE_SEC_ACT sec_act{BTM_BLE_SEC_ENCRYPT};

  // No device
  ASSERT_EQ(BTM_WRONG_MODE, BTM_SetEncryption(bd_addr, transport, p_callback,
                                              nullptr, sec_act));

  // With device
  tBTM_SEC_DEV_REC* device_record = btm_sec_allocate_dev_rec();
  ASSERT_NE(nullptr, device_record);
  device_record->bd_addr = bd_addr;
  device_record->hci_handle = 0x1234;

  ASSERT_EQ(BTM_WRONG_MODE, BTM_SetEncryption(bd_addr, transport, p_callback,
                                              nullptr, sec_act));

  wipe_secrets_and_remove(device_record);
}

TEST_F(StackBtmTest, sco_state_text) {
  std::vector<std::pair<tSCO_STATE, std::string>> states = {
      std::make_pair(SCO_ST_UNUSED, "SCO_ST_UNUSED"),
      std::make_pair(SCO_ST_LISTENING, "SCO_ST_LISTENING"),
      std::make_pair(SCO_ST_W4_CONN_RSP, "SCO_ST_W4_CONN_RSP"),
      std::make_pair(SCO_ST_CONNECTING, "SCO_ST_CONNECTING"),
      std::make_pair(SCO_ST_CONNECTED, "SCO_ST_CONNECTED"),
      std::make_pair(SCO_ST_DISCONNECTING, "SCO_ST_DISCONNECTING"),
      std::make_pair(SCO_ST_PEND_UNPARK, "SCO_ST_PEND_UNPARK"),
      std::make_pair(SCO_ST_PEND_ROLECHANGE, "SCO_ST_PEND_ROLECHANGE"),
      std::make_pair(SCO_ST_PEND_MODECHANGE, "SCO_ST_PEND_MODECHANGE"),
  };
  for (const auto& state : states) {
    ASSERT_STREQ(state.second.c_str(), sco_state_text(state.first).c_str());
  }
  std::ostringstream oss;
  oss << "unknown_sco_state: " << std::numeric_limits<std::uint16_t>::max();
  ASSERT_STREQ(oss.str().c_str(),
               sco_state_text(static_cast<tSCO_STATE>(
                                  std::numeric_limits<std::uint16_t>::max()))
                   .c_str());
}

TEST_F(StackBtmTest, btm_ble_sec_req_act_text) {
  ASSERT_EQ("BTM_BLE_SEC_REQ_ACT_NONE",
            btm_ble_sec_req_act_text(BTM_BLE_SEC_REQ_ACT_NONE));
  ASSERT_EQ("BTM_BLE_SEC_REQ_ACT_ENCRYPT",
            btm_ble_sec_req_act_text(BTM_BLE_SEC_REQ_ACT_ENCRYPT));
  ASSERT_EQ("BTM_BLE_SEC_REQ_ACT_PAIR",
            btm_ble_sec_req_act_text(BTM_BLE_SEC_REQ_ACT_PAIR));
  ASSERT_EQ("BTM_BLE_SEC_REQ_ACT_DISCARD",
            btm_ble_sec_req_act_text(BTM_BLE_SEC_REQ_ACT_DISCARD));
}

TEST_F(StackBtmWithInitFreeTest, btm_sec_allocate_dev_rec__all) {
  tBTM_SEC_DEV_REC* records[kBtmSecMaxDeviceRecords];

  // Fill up the records
  for (size_t i = 0; i < kBtmSecMaxDeviceRecords; i++) {
    ASSERT_EQ(i, list_length(btm_cb.sec_dev_rec));
    records[i] = btm_sec_allocate_dev_rec();
    ASSERT_NE(nullptr, records[i]);
  }

  // Second pass up the records
  for (size_t i = 0; i < kBtmSecMaxDeviceRecords; i++) {
    ASSERT_EQ(kBtmSecMaxDeviceRecords, list_length(btm_cb.sec_dev_rec));
    records[i] = btm_sec_allocate_dev_rec();
    ASSERT_NE(nullptr, records[i]);
  }

  // NOTE: The memory allocated for each record is automatically
  // allocated by the btm module and freed when the device record
  // list is freed.
  // Further, the memory for each record is reused when necessary.
}

TEST_F(StackBtmTest, btm_oob_data_text) {
  std::vector<std::pair<tBTM_OOB_DATA, std::string>> datas = {
      std::make_pair(BTM_OOB_NONE, "BTM_OOB_NONE"),
      std::make_pair(BTM_OOB_PRESENT_192, "BTM_OOB_PRESENT_192"),
      std::make_pair(BTM_OOB_PRESENT_256, "BTM_OOB_PRESENT_256"),
      std::make_pair(BTM_OOB_PRESENT_192_AND_256,
                     "BTM_OOB_PRESENT_192_AND_256"),
      std::make_pair(BTM_OOB_UNKNOWN, "BTM_OOB_UNKNOWN"),
  };
  for (const auto& data : datas) {
    ASSERT_STREQ(data.second.c_str(), btm_oob_data_text(data.first).c_str());
  }
  auto unknown = base::StringPrintf("UNKNOWN[%hhu]",
                                    std::numeric_limits<std::uint8_t>::max());
  ASSERT_STREQ(unknown.c_str(),
               btm_oob_data_text(static_cast<tBTM_OOB_DATA>(
                                     std::numeric_limits<std::uint8_t>::max()))
                   .c_str());
}

TEST_F(StackBtmTest, bond_type_text) {
  std::vector<std::pair<tBTM_SEC_DEV_REC::tBTM_BOND_TYPE, std::string>> datas =
      {
          std::make_pair(tBTM_SEC_DEV_REC::BOND_TYPE_UNKNOWN,
                         "tBTM_SEC_DEV_REC::BOND_TYPE_UNKNOWN"),
          std::make_pair(tBTM_SEC_DEV_REC::BOND_TYPE_PERSISTENT,
                         "tBTM_SEC_DEV_REC::BOND_TYPE_PERSISTENT"),
          std::make_pair(tBTM_SEC_DEV_REC::BOND_TYPE_TEMPORARY,
                         "tBTM_SEC_DEV_REC::BOND_TYPE_TEMPORARY"),
      };
  for (const auto& data : datas) {
    ASSERT_STREQ(data.second.c_str(), bond_type_text(data.first).c_str());
  }
  auto unknown = base::StringPrintf("UNKNOWN[%hhu]",
                                    std::numeric_limits<std::uint8_t>::max());
  ASSERT_STREQ(unknown.c_str(),
               bond_type_text(static_cast<tBTM_SEC_DEV_REC::tBTM_BOND_TYPE>(
                                  std::numeric_limits<std::uint8_t>::max()))
                   .c_str());
}

TEST_F(StackBtmWithInitFreeTest, wipe_secrets_and_remove) {
  bluetooth::common::InitFlags::SetAllForTesting();

  RawAddress bd_addr = RawAddress({0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6});
  const uint16_t classic_handle = 0x1234;
  const uint16_t ble_handle = 0x9876;

  // Setup device
  tBTM_SEC_DEV_REC* device_record = btm_sec_allocate_dev_rec();
  ASSERT_NE(nullptr, device_record);
  ASSERT_EQ(BTM_SEC_IN_USE, device_record->sec_flags);
  device_record->bd_addr = bd_addr;
  device_record->hci_handle = classic_handle;
  device_record->ble_hci_handle = ble_handle;

  wipe_secrets_and_remove(device_record);
}

/*
 * Copyright 2021 HIMSA II K/S - www.himsa.com.
 * Represented by EHIMA - www.ehima.com
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <base/bind.h>
#include <base/logging.h>
#include <base/strings/string_number_conversions.h>
#include <hardware/bt_vc.h>

#include <string>
#include <vector>

#include "bind_helpers.h"
#include "bta/le_audio/le_audio_types.h"
#include "bta_csis_api.h"
#include "bta_gatt_api.h"
#include "bta_gatt_queue.h"
#include "bta_vc_api.h"
#include "btif_storage.h"
#include "devices.h"
#include "osi/include/osi.h"
#include "types/bluetooth/uuid.h"
#include "types/raw_address.h"

using base::Closure;
using bluetooth::Uuid;
using bluetooth::csis::CsisClient;
using bluetooth::vc::ConnectionState;
using namespace bluetooth::vc::internal;

namespace {
class VolumeControlImpl;
VolumeControlImpl* instance;

/**
 * Overview:
 *
 * This is Volume Control Implementation class which realize Volume Control
 * Profile (VCP)
 *
 * Each connected peer device supporting Volume Control Service (VCS) is on the
 * list of devices (volume_control_devices_).
 *
 * Once all the mandatory characteristis for all the services are discovered,
 * Fluoride calls ON_CONNECTED callback.
 *
 * It is assumed that whenever application changes general audio options in this
 * profile e.g. Volume up/down, mute/unmute etc, profile configures all the
 * devices which are active Le Audio devices.
 *
 *
 */
class VolumeControlImpl : public VolumeControl {
 public:
  ~VolumeControlImpl() override = default;

  VolumeControlImpl(bluetooth::vc::VolumeControlCallbacks* callbacks)
      : gatt_if_(0), callbacks_(callbacks), latest_operation_id_(0) {
    BTA_GATTC_AppRegister(
        gattc_callback_static,
        base::Bind([](uint8_t client_id, uint8_t status) {
          if (status != GATT_SUCCESS) {
            LOG(ERROR) << "Can't start Volume Control profile - no gatt "
                          "clients left!";
            return;
          }
          instance->gatt_if_ = client_id;
        }),
        true);
  }

  void Connect(const RawAddress& address) override {
    LOG(INFO) << __func__ << " " << address;

    auto device = volume_control_devices_.FindByAddress(address);
    if (!device) {
      volume_control_devices_.Add(address, true);
    } else {
      device->connecting_actively = true;
    }

    BTA_GATTC_Open(gatt_if_, address, true, false);
  }

  void AddFromStorage(const RawAddress& address, bool auto_connect) {
    LOG(INFO) << __func__ << " " << address
              << ", auto_connect=" << auto_connect;

    if (auto_connect) {
      volume_control_devices_.Add(address, false);

      /* Add device into BG connection to accept remote initiated connection */
      BTA_GATTC_Open(gatt_if_, address, false, false);
    }
  }

  void OnGattConnected(tGATT_STATUS status, uint16_t connection_id,
                       tGATT_IF /*client_if*/, RawAddress address,
                       tBT_TRANSPORT /*transport*/, uint16_t /*mtu*/) {
    LOG(INFO) << __func__ << ": address=" << address
              << ", connection_id=" << connection_id;

    VolumeControlDevice* device =
        volume_control_devices_.FindByAddress(address);
    if (!device) {
      LOG(ERROR) << __func__ << "Skipping unknown device, address=" << address;
      return;
    }

    if (status != GATT_SUCCESS) {
      LOG(INFO) << "Failed to connect to Volume Control device";
      device_cleanup_helper(device, device->connecting_actively);
      return;
    }

    device->connection_id = connection_id;

    if (device->IsEncryptionEnabled()) {
      OnEncryptionComplete(address, BTM_SUCCESS);
      return;
    }

    if (!device->EnableEncryption(enc_callback_static)) {
      device_cleanup_helper(device, device->connecting_actively);
    }
  }

  void OnEncryptionComplete(const RawAddress& address, uint8_t success) {
    VolumeControlDevice* device =
        volume_control_devices_.FindByAddress(address);
    if (!device) {
      LOG(ERROR) << __func__ << "Skipping unknown device " << address;
      return;
    }

    if (success != BTM_SUCCESS) {
      LOG(ERROR) << "encryption failed "
                 << "status: " << int{success};
      // If the encryption failed, do not remove the device.
      // Disconnect only, since the Android will try to re-enable encryption
      // after disconnection
      device->Disconnect(gatt_if_);
      if (device->connecting_actively)
        callbacks_->OnConnectionState(ConnectionState::DISCONNECTED,
                                      device->address);
      return;
    }

    LOG(INFO) << __func__ << " " << address << " status: " << +success;

    if (device->HasHandles()) {
      device->EnqueueInitialRequests(gatt_if_, chrc_read_callback_static,
                                     OnGattWriteCccStatic);

    } else {
      device->first_connection = true;
      BTA_GATTC_ServiceSearchRequest(device->connection_id,
                                     &kVolumeControlUuid);
    }
  }

  void OnServiceChangeEvent(const RawAddress& address) {
    VolumeControlDevice* device =
        volume_control_devices_.FindByAddress(address);
    if (!device) {
      LOG(ERROR) << __func__ << "Skipping unknown device " << address;
      return;
    }
    LOG(INFO) << __func__ << ": address=" << address;
    device->first_connection = true;
    device->service_changed_rcvd = true;
    BtaGattQueue::Clean(device->connection_id);
  }

  void OnServiceDiscDoneEvent(const RawAddress& address) {
    VolumeControlDevice* device =
        volume_control_devices_.FindByAddress(address);
    if (!device) {
      LOG(ERROR) << __func__ << "Skipping unknown device " << address;
      return;
    }

    if (device->service_changed_rcvd)
      BTA_GATTC_ServiceSearchRequest(device->connection_id,
                                     &kVolumeControlUuid);
  }

  void OnServiceSearchComplete(uint16_t connection_id, tGATT_STATUS status) {
    VolumeControlDevice* device =
        volume_control_devices_.FindByConnId(connection_id);
    if (!device) {
      LOG(ERROR) << __func__ << "Skipping unknown device, connection_id="
                 << loghex(connection_id);
      return;
    }

    /* Known device, nothing to do */
    if (!device->first_connection) return;

    if (status != GATT_SUCCESS) {
      /* close connection and report service discovery complete with error */
      LOG(ERROR) << "Service discovery failed";
      device_cleanup_helper(device, device->first_connection);
      return;
    }

    bool success = device->UpdateHandles();
    if (!success) {
      LOG(ERROR) << "Incomplete service database";
      device_cleanup_helper(device, true);
      return;
    }

    device->EnqueueInitialRequests(gatt_if_, chrc_read_callback_static,
                                   OnGattWriteCccStatic);
  }

  void OnCharacteristicValueChanged(uint16_t conn_id, tGATT_STATUS status,
                                    uint16_t handle, uint16_t len,
                                    uint8_t* value, void* data,
                                    bool is_notification) {
    VolumeControlDevice* device = volume_control_devices_.FindByConnId(conn_id);
    if (!device) {
      LOG(INFO) << __func__ << ": unknown conn_id=" << loghex(conn_id);
      return;
    }

    if (status != GATT_SUCCESS) {
      LOG(INFO) << __func__ << ": status=" << static_cast<int>(status);
      return;
    }

    if (handle == device->volume_state_handle) {
      OnVolumeControlStateReadOrNotified(device, len, value, is_notification);
      verify_device_ready(device, handle);
      return;
    }
    if (handle == device->volume_flags_handle) {
      OnVolumeControlFlagsChanged(device, len, value);
      verify_device_ready(device, handle);
      return;
    }

    LOG(ERROR) << __func__ << ": unknown handle=" << loghex(handle);
  }

  void OnNotificationEvent(uint16_t conn_id, uint16_t handle, uint16_t len,
                           uint8_t* value) {
    LOG(INFO) << __func__ << ": handle=" << loghex(handle);
    OnCharacteristicValueChanged(conn_id, GATT_SUCCESS, handle, len, value,
                                 nullptr, true);
  }

  void VolumeControlReadCommon(uint16_t conn_id, uint16_t handle) {
    BtaGattQueue::ReadCharacteristic(conn_id, handle, chrc_read_callback_static,
                                     nullptr);
  }

  void HandleAutonomusVolumeChange(VolumeControlDevice* device,
                                   bool is_volume_change, bool is_mute_change) {
    DLOG(INFO) << __func__ << device->address
               << " is volume change: " << is_volume_change
               << " is mute change: " << is_mute_change;

    if (!is_volume_change && !is_mute_change) {
      LOG(ERROR) << __func__
                 << "Autonomous change but volume and mute did not changed.";
      return;
    }

    auto csis_api = CsisClient::Get();
    if (!csis_api) {
      DLOG(INFO) << __func__ << " Csis is not available";
      callbacks_->OnVolumeStateChanged(device->address, device->volume,
                                       device->mute);
      return;
    }

    auto group_id =
        csis_api->GetGroupId(device->address, le_audio::uuid::kCapServiceUuid);
    if (group_id == bluetooth::groups::kGroupUnknown) {
      DLOG(INFO) << __func__ << " No group for device " << device->address;
      callbacks_->OnVolumeStateChanged(device->address, device->volume,
                                       device->mute);
      return;
    }

    auto devices = csis_api->GetDeviceList(group_id);
    for (auto it = devices.begin(); it != devices.end();) {
      auto dev = volume_control_devices_.FindByAddress(*it);
      if (!dev || !dev->IsConnected() || (dev->address == device->address)) {
        it = devices.erase(it);
      } else {
        it++;
      }
    }

    if (is_volume_change) {
      std::vector<uint8_t> arg({device->volume});
      PrepareVolumeControlOperation(devices, group_id,
                                    kControlPointOpcodeSetAbsoluteVolume, arg);
    }

    if (is_mute_change) {
      std::vector<uint8_t> arg;
      uint8_t opcode =
          device->mute ? kControlPointOpcodeMute : kControlPointOpcodeUnmute;
      PrepareVolumeControlOperation(devices, group_id, opcode, arg);
    }

    StartQueueOperation();
  }

  void OnVolumeControlStateReadOrNotified(VolumeControlDevice* device,
                                          uint16_t len, uint8_t* value,
                                          bool is_notification) {
    if (len != 3) {
      LOG(INFO) << __func__ << ": malformed len=" << loghex(len);
      return;
    }

    uint8_t vol;
    uint8_t mute;
    uint8_t* pp = value;
    STREAM_TO_UINT8(vol, pp);
    STREAM_TO_UINT8(mute, pp);
    STREAM_TO_UINT8(device->change_counter, pp);

    bool is_volume_change = (device->volume != vol);
    device->volume = vol;

    bool is_mute_change = (device->mute != mute);
    device->mute = mute;

    LOG(INFO) << __func__ << " volume " << loghex(device->volume) << " mute "
              << loghex(device->mute) << " change_counter "
              << loghex(device->change_counter);

    if (!device->device_ready) return;

    /* This is just a read, send single notification */
    if (!is_notification) {
      callbacks_->OnVolumeStateChanged(device->address, device->volume,
                                       device->mute);
      return;
    }

    auto addr = device->address;
    auto op = find_if(ongoing_operations_.begin(), ongoing_operations_.end(),
                      [addr](auto& operation) {
                        auto it = find(operation.devices_.begin(),
                                       operation.devices_.end(), addr);
                        return it != operation.devices_.end();
                      });
    if (op == ongoing_operations_.end()) {
      DLOG(INFO) << __func__ << " Could not find operation id for device: "
                 << device->address << ". Autonomus change";
      HandleAutonomusVolumeChange(device, is_volume_change, is_mute_change);
      return;
    }

    DLOG(INFO) << __func__ << " operation found: " << op->operation_id_
               << " for group id: " << op->group_id_;

    /* Received notification from the device we do expect */
    auto it = find(op->devices_.begin(), op->devices_.end(), device->address);
    op->devices_.erase(it);
    if (!op->devices_.empty()) {
      DLOG(INFO) << __func__ << " wait for more responses for operation_id: "
                 << op->operation_id_;
      return;
    }

    if (op->IsGroupOperation())
      callbacks_->OnGroupVolumeStateChanged(op->group_id_, device->volume,
                                            device->mute);
    else
      callbacks_->OnVolumeStateChanged(device->address, device->volume,
                                       device->mute);

    ongoing_operations_.erase(op);
    StartQueueOperation();
  }

  void OnVolumeControlFlagsChanged(VolumeControlDevice* device, uint16_t len,
                                   uint8_t* value) {
    device->flags = *value;

    LOG(INFO) << __func__ << " flags " << loghex(device->flags);
  }

  void OnGattWriteCcc(uint16_t connection_id, tGATT_STATUS status,
                      uint16_t handle, uint16_t len, const uint8_t* value,
                      void* /*data*/) {
    VolumeControlDevice* device =
        volume_control_devices_.FindByConnId(connection_id);
    if (!device) {
      LOG(INFO) << __func__
                << "unknown connection_id=" << loghex(connection_id);
      BtaGattQueue::Clean(connection_id);
      return;
    }

    if (status != GATT_SUCCESS) {
      LOG(ERROR) << __func__
                 << "Failed to register for notification: " << loghex(handle)
                 << " status: " << status;
      device_cleanup_helper(device, true);
      return;
    }

    LOG(INFO) << __func__
              << "Successfully register for indications: " << loghex(handle);

    verify_device_ready(device, handle);
  }

  static void OnGattWriteCccStatic(uint16_t connection_id, tGATT_STATUS status,
                                   uint16_t handle, uint16_t len,
                                   const uint8_t* value, void* data) {
    if (!instance) {
      LOG(ERROR) << __func__ << "No instance=" << handle;
      return;
    }

    instance->OnGattWriteCcc(connection_id, status, handle, len, value, data);
  }

  void Dump(int fd) { volume_control_devices_.DebugDump(fd); }

  void Disconnect(const RawAddress& address) override {
    VolumeControlDevice* device =
        volume_control_devices_.FindByAddress(address);
    if (!device) {
      LOG(INFO) << "Device not connected to profile " << address;
      return;
    }

    LOG(INFO) << __func__ << " GAP_EVT_CONN_CLOSED: " << device->address;
    device_cleanup_helper(device, true);
  }

  void OnGattDisconnected(uint16_t connection_id, tGATT_IF /*client_if*/,
                          RawAddress remote_bda,
                          tGATT_DISCONN_REASON /*reason*/) {
    VolumeControlDevice* device =
        volume_control_devices_.FindByConnId(connection_id);
    if (!device) {
      LOG(ERROR) << __func__
                 << " Skipping unknown device disconnect, connection_id="
                 << loghex(connection_id);
      return;
    }

    // If we get here, it means, device has not been exlicitly disconnected.
    bool device_ready = device->device_ready;

    device_cleanup_helper(device, device->connecting_actively);

    if (device_ready) {
      volume_control_devices_.Add(remote_bda, true);

      /* Add device into BG connection to accept remote initiated connection */
      BTA_GATTC_Open(gatt_if_, remote_bda, false, false);
    }
  }

  void RemoveDeviceFromOperationList(const RawAddress& addr, int operation_id) {
    auto op = find_if(ongoing_operations_.begin(), ongoing_operations_.end(),
                      [operation_id](auto& operation) {
                        return operation.operation_id_ == operation_id;
                      });

    if (op == ongoing_operations_.end()) {
      LOG(ERROR) << __func__
                 << " Could not find operation id: " << operation_id;
      return;
    }

    auto it = find(op->devices_.begin(), op->devices_.end(), addr);
    if (it != op->devices_.end()) {
      op->devices_.erase(it);
      if (op->devices_.empty()) {
        ongoing_operations_.erase(op);
        StartQueueOperation();
      }
      return;
    }
  }

  void OnWriteControlResponse(uint16_t connection_id, tGATT_STATUS status,
                              uint16_t handle, void* data) {
    VolumeControlDevice* device =
        volume_control_devices_.FindByConnId(connection_id);
    if (!device) {
      LOG(ERROR) << __func__
                 << "Skipping unknown device disconnect, connection_id="
                 << loghex(connection_id);
      return;
    }

    LOG(INFO) << "Write response handle: " << loghex(handle)
              << " status: " << loghex((int)(status));

    if (status == GATT_SUCCESS) return;

    /* In case of error, remove device from the tracking operation list */
    RemoveDeviceFromOperationList(device->address, PTR_TO_INT(data));
  }

  static void operation_callback(void* data) {
    instance->CancelVolumeOperation(PTR_TO_INT(data));
  }

  void StartQueueOperation(void) {
    LOG(INFO) << __func__;
    if (ongoing_operations_.empty()) {
      return;
    };

    auto op = &ongoing_operations_.front();

    LOG(INFO) << __func__ << " operation_id: " << op->operation_id_;

    if (op->IsStarted()) {
      LOG(INFO) << __func__ << " wait until operation " << op->operation_id_
                << " is complete";
      return;
    }

    op->Start();

    alarm_set_on_mloop(op->operation_timeout_, 3000, operation_callback,
                       INT_TO_PTR(op->operation_id_));
    devices_control_point_helper(
        op->devices_, op->opcode_,
        op->arguments_.size() == 0 ? nullptr : &(op->arguments_));
  }

  void CancelVolumeOperation(int operation_id) {
    LOG(INFO) << __func__ << " canceling operation_id: " << operation_id;

    auto op = find_if(
        ongoing_operations_.begin(), ongoing_operations_.end(),
        [operation_id](auto& it) { return it.operation_id_ == operation_id; });

    if (op == ongoing_operations_.end()) {
      LOG(ERROR) << __func__
                 << " Could not find operation_id: " << operation_id;
      return;
    }

    /* Possibly close GATT operations */
    ongoing_operations_.erase(op);
    StartQueueOperation();
  }

  void ProceedVolumeOperation(int operation_id) {
    auto op = find_if(ongoing_operations_.begin(), ongoing_operations_.end(),
                      [operation_id](auto& operation) {
                        return operation.operation_id_ == operation_id;
                      });

    DLOG(INFO) << __func__ << " operation_id: " << operation_id;

    if (op == ongoing_operations_.end()) {
      LOG(ERROR) << __func__
                 << " Could not find operation_id: " << operation_id;
      return;
    }

    DLOG(INFO) << __func__ << " procedure continued for operation_id: "
               << op->operation_id_;

    alarm_set_on_mloop(op->operation_timeout_, 3000, operation_callback,
                       INT_TO_PTR(op->operation_id_));
    devices_control_point_helper(op->devices_, op->opcode_, &(op->arguments_));
  }

  void PrepareVolumeControlOperation(std::vector<RawAddress>& devices,
                                     int group_id, uint8_t opcode,
                                     std::vector<uint8_t>& arguments) {
    DLOG(INFO) << __func__ << " num of devices: " << devices.size()
               << " group_id: " << group_id << " opcode: " << +opcode
               << " arg size: " << arguments.size();

    ongoing_operations_.emplace_back(latest_operation_id_++, group_id, opcode,
                                     arguments, devices);
  }

  void SetVolume(std::variant<RawAddress, int> addr_or_group_id,
                 uint8_t volume) override {
    DLOG(INFO) << __func__ << " vol: " << +volume;

    std::vector<uint8_t> arg({volume});
    uint8_t opcode = kControlPointOpcodeSetAbsoluteVolume;

    if (std::holds_alternative<RawAddress>(addr_or_group_id)) {
      DLOG(INFO) << __func__ << " " << std::get<RawAddress>(addr_or_group_id);
      std::vector<RawAddress> devices = {
          std::get<RawAddress>(addr_or_group_id)};

      PrepareVolumeControlOperation(devices, bluetooth::groups::kGroupUnknown,
                                    opcode, arg);
    } else {
      /* Handle group change */
      auto group_id = std::get<int>(addr_or_group_id);
      DLOG(INFO) << __func__ << " group: " << group_id;
      auto csis_api = CsisClient::Get();
      if (!csis_api) {
        LOG(ERROR) << __func__ << " Csis is not there";
        return;
      }

      auto devices = csis_api->GetDeviceList(group_id);
      for (auto it = devices.begin(); it != devices.end();) {
        auto dev = volume_control_devices_.FindByAddress(*it);
        if (!dev || !dev->IsConnected()) {
          it = devices.erase(it);
        } else {
          it++;
        }
      }

      if (devices.empty()) {
        LOG(ERROR) << __func__ << " group id : " << group_id
                   << " is not connected? ";
        return;
      }

      PrepareVolumeControlOperation(devices, group_id, opcode, arg);
    }

    StartQueueOperation();
  }

  void CleanUp() {
    LOG(INFO) << __func__;
    volume_control_devices_.Disconnect(gatt_if_);
    volume_control_devices_.Clear();
    ongoing_operations_.clear();
    BTA_GATTC_AppDeregister(gatt_if_);
  }

 private:
  tGATT_IF gatt_if_;
  bluetooth::vc::VolumeControlCallbacks* callbacks_;
  VolumeControlDevices volume_control_devices_;

  /* Used to track volume control operations */
  std::list<VolumeOperation> ongoing_operations_;
  int latest_operation_id_;

  void verify_device_ready(VolumeControlDevice* device, uint16_t handle) {
    if (device->device_ready) return;

    // VerifyReady sets the device_ready flag if all remaining GATT operations
    // are completed
    if (device->VerifyReady(handle)) {
      LOG(INFO) << __func__ << " Outstanding reads completed.";

      callbacks_->OnConnectionState(ConnectionState::CONNECTED,
                                    device->address);

      device->connecting_actively = true;

      device->first_connection = false;

      // once profile connected we can notify current states
      callbacks_->OnVolumeStateChanged(device->address, device->volume,
                                       device->mute);

      device->EnqueueRemainingRequests(gatt_if_, chrc_read_callback_static,
                                       OnGattWriteCccStatic);
    }
  }

  void device_cleanup_helper(VolumeControlDevice* device, bool notify) {
    device->Disconnect(gatt_if_);
    if (notify)
      callbacks_->OnConnectionState(ConnectionState::DISCONNECTED,
                                    device->address);
    volume_control_devices_.Remove(device->address);
  }

  void devices_control_point_helper(std::vector<RawAddress>& devices,
                                    uint8_t opcode,
                                    const std::vector<uint8_t>* arg,
                                    int operation_id = -1) {
    volume_control_devices_.ControlPointOperation(
        devices, opcode, arg,
        [](uint16_t connection_id, tGATT_STATUS status, uint16_t handle,
           uint16_t len, const uint8_t* value, void* data) {
          if (instance)
            instance->OnWriteControlResponse(connection_id, status, handle,
                                             data);
        },
        INT_TO_PTR(operation_id));
  }

  void gattc_callback(tBTA_GATTC_EVT event, tBTA_GATTC* p_data) {
    LOG(INFO) << __func__ << " event = " << static_cast<int>(event);

    if (p_data == nullptr) return;

    switch (event) {
      case BTA_GATTC_OPEN_EVT: {
        tBTA_GATTC_OPEN& o = p_data->open;
        OnGattConnected(o.status, o.conn_id, o.client_if, o.remote_bda,
                        o.transport, o.mtu);

      } break;

      case BTA_GATTC_CLOSE_EVT: {
        tBTA_GATTC_CLOSE& c = p_data->close;
        OnGattDisconnected(c.conn_id, c.client_if, c.remote_bda, c.reason);
      } break;

      case BTA_GATTC_SEARCH_CMPL_EVT:
        OnServiceSearchComplete(p_data->search_cmpl.conn_id,
                                p_data->search_cmpl.status);
        break;

      case BTA_GATTC_NOTIF_EVT: {
        tBTA_GATTC_NOTIFY& n = p_data->notify;
        if (!n.is_notify || n.len > GATT_MAX_ATTR_LEN) {
          LOG(ERROR) << __func__ << ": rejected BTA_GATTC_NOTIF_EVT. is_notify="
                     << n.is_notify << ", len=" << static_cast<int>(n.len);
          break;
        }
        OnNotificationEvent(n.conn_id, n.handle, n.len, n.value);
      } break;

      case BTA_GATTC_ENC_CMPL_CB_EVT:
        OnEncryptionComplete(p_data->enc_cmpl.remote_bda, BTM_SUCCESS);
        break;

      case BTA_GATTC_SRVC_CHG_EVT:
        OnServiceChangeEvent(p_data->remote_bda);
        break;

      case BTA_GATTC_SRVC_DISC_DONE_EVT:
        OnServiceDiscDoneEvent(p_data->remote_bda);
        break;

      default:
        break;
    }
  }

  static void gattc_callback_static(tBTA_GATTC_EVT event, tBTA_GATTC* p_data) {
    if (instance) instance->gattc_callback(event, p_data);
  }

  static void enc_callback_static(const RawAddress* address, tBT_TRANSPORT,
                                  void*, tBTM_STATUS status) {
    if (instance) instance->OnEncryptionComplete(*address, status);
  }

  static void chrc_read_callback_static(uint16_t conn_id, tGATT_STATUS status,
                                        uint16_t handle, uint16_t len,
                                        uint8_t* value, void* data) {
    if (instance)
      instance->OnCharacteristicValueChanged(conn_id, status, handle, len,
                                             value, data, false);
  }
};
}  // namespace

void VolumeControl::Initialize(
    bluetooth::vc::VolumeControlCallbacks* callbacks) {
  if (instance) {
    LOG(ERROR) << "Already initialized!";
    return;
  }

  instance = new VolumeControlImpl(callbacks);
}

bool VolumeControl::IsVolumeControlRunning() { return instance; }

VolumeControl* VolumeControl::Get(void) {
  CHECK(instance);
  return instance;
};

void VolumeControl::AddFromStorage(const RawAddress& address,
                                   bool auto_connect) {
  if (!instance) {
    LOG(ERROR) << "Not initialized yet";
    return;
  }

  instance->AddFromStorage(address, auto_connect);
};

void VolumeControl::CleanUp() {
  if (!instance) {
    LOG(ERROR) << "Not initialized!";
    return;
  }

  VolumeControlImpl* ptr = instance;
  instance = nullptr;

  ptr->CleanUp();

  delete ptr;
};

void VolumeControl::DebugDump(int fd) {
  dprintf(fd, "Volume Control Manager:\n");
  if (instance) instance->Dump(fd);
  dprintf(fd, "\n");
}

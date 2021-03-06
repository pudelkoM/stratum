// Copyright 2018 Google LLC
// Copyright 2018-present Open Networking Foundation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "stratum/hal/lib/phal/onlp/onlpphal.h"

#include <string>

#include "stratum/glue/status/status.h"
#include "stratum/glue/status/status_macros.h"
#include "stratum/glue/status/statusor.h"
#include "stratum/hal/lib/common/constants.h"
#include "stratum/hal/lib/phal/onlp/onlp_wrapper.h"
#include "stratum/lib/macros.h"
#include "stratum/hal/lib/phal/attribute_database.h"
#include "stratum/hal/lib/phal/onlp/switch_configurator.h"

DEFINE_int32(max_num_transceiver_writers, 2,
             "Maximum number of channel writers for transceiver events.");

namespace stratum {
namespace hal {
namespace phal {
namespace onlp {

using TransceiverEvent = PhalInterface::TransceiverEvent;
using TransceiverEventWriter = PhalInterface::TransceiverEventWriter;

OnlpPhal* OnlpPhal::singleton_ = nullptr;
ABSL_CONST_INIT absl::Mutex OnlpPhal::init_lock_(absl::kConstInit);

::util::Status OnlpPhalSfpEventCallback::HandleStatusChange(
    const OidInfo& oid_info) {
  // Check OID Type
  switch (oid_info.GetType()) {
    // SFP event
    case ONLP_OID_TYPE_SFP:
      // Format TransceiverEvent
      TransceiverEvent event;
      event.slot = kDefaultSlot;
      event.port = oid_info.GetId();
      event.state = oid_info.GetHardwareState();
      RETURN_IF_ERROR(onlpphal_->HandleTransceiverEvent(event));
      break;

    // TODO(craig): we probably need to handle more than just
    //              transceiver events over time.
    default:
      return MAKE_ERROR() << "unhandled status change, oid: "
                          << oid_info.GetHeader()->id;
  }

  return ::util::OkStatus();
}

OnlpPhal::OnlpPhal()
    : onlp_interface_(nullptr),
      onlp_event_handler_(nullptr),
      sfp_event_callback_(nullptr) {}

OnlpPhal::~OnlpPhal() {}

// Initialize the onlp interface and phal DB
::util::Status OnlpPhal::Initialize() {
  absl::WriterMutexLock l(&config_lock_);

  if (!initialized_) {
    // Create the OnlpWrapper object
    RETURN_IF_ERROR(InitializeOnlpInterface());

    // Create attribute database and load initial phal DB
    RETURN_IF_ERROR(InitializePhalDB());

    // Create the OnlpEventHandler object with given OnlpWrapper
    RETURN_IF_ERROR(InitializeOnlpEventHandler());

    initialized_ = true;
  }
  return ::util::OkStatus();
}

::util::Status OnlpPhal::InitializePhalDB() {
  // Create onlp switch configurator instance
  ASSIGN_OR_RETURN(auto configurator,
                   OnlpSwitchConfigurator::Make(this, onlp_interface_.get()));

  // Create attribute database and load initial phal DB
  ASSIGN_OR_RETURN(std::move(database_),
                   AttributeDatabase::MakePhalDB(std::move(configurator)));

  return ::util::OkStatus();
}

::util::Status OnlpPhal::PushChassisConfig(const ChassisConfig& config) {
  absl::WriterMutexLock l(&config_lock_);

  // TODO(unknown): Process Chassis Config here

  return ::util::OkStatus();
}

::util::Status OnlpPhal::VerifyChassisConfig(const ChassisConfig& config) {
  // TODO(unknown): Implement this function.
  return ::util::OkStatus();
}

::util::Status OnlpPhal::Shutdown() {
  absl::WriterMutexLock l(&config_lock_);

  // TODO(unknown): add clean up code

  initialized_ = false;

  return ::util::OkStatus();
}

::util::Status OnlpPhal::HandleTransceiverEvent(const TransceiverEvent& event) {
  // Send event to Sfp configurator first to ensure
  // attribute database is in order before and calls are
  // made from the upper layer components.
  const std::pair<int, int>& slot_port_pair =
      std::make_pair(event.slot, event.port);
  auto configurator = slot_port_to_configurator_[slot_port_pair];

  // Check to make sure we've got a configurator for this slot/port
  if (configurator == nullptr) {
    RETURN_ERROR() << "card[" << event.slot << "]/port[" << event.port << "]: "
                   << "no configurator for this transceiver";
  }

  RETURN_IF_ERROR(configurator->HandleEvent(event.state));

  // Write the event to each writer
  return WriteTransceiverEvent(event);
}

::util::StatusOr<int> OnlpPhal::RegisterTransceiverEventWriter(
    std::unique_ptr<ChannelWriter<TransceiverEvent>> writer, int priority) {
  absl::WriterMutexLock l(&config_lock_);
  if (!initialized_) {
    return MAKE_ERROR(ERR_NOT_INITIALIZED) << "Not initialized!";
  }

  CHECK_RETURN_IF_FALSE(transceiver_event_writers_.size() <
                        static_cast<size_t>(FLAGS_max_num_transceiver_writers))
      << "Can only support " << FLAGS_max_num_transceiver_writers
      << " transceiver event Writers.";

  // Find the next available ID for the Writer.
  int next_id = kInvalidWriterId;
  for (int id = 1;
       id <= static_cast<int>(transceiver_event_writers_.size()) + 1; ++id) {
    auto it = std::find_if(
        transceiver_event_writers_.begin(), transceiver_event_writers_.end(),
        [id](const TransceiverEventWriter& w) { return w.id == id; });
    if (it == transceiver_event_writers_.end()) {
      // This id is free. Pick it up.
      next_id = id;
      break;
    }
  }
  CHECK_RETURN_IF_FALSE(next_id != kInvalidWriterId)
      << "Could not find a new ID for the Writer. next_id=" << next_id << ".";

  transceiver_event_writers_.insert({std::move(writer), priority, next_id});

  // Note: register callback only after writer registered. Only register
  //       callback once.
  if (sfp_event_callback_ == nullptr) {
    // Create OnlpSfpEventCallback
    std::unique_ptr<OnlpPhalSfpEventCallback> callback(
        new OnlpPhalSfpEventCallback());
    sfp_event_callback_ = std::move(callback);
    sfp_event_callback_->onlpphal_ = this;

    // Register OnlpSfpEventCallback
    ::util::Status result = onlp_event_handler_->RegisterSfpEventCallback(
        sfp_event_callback_.get());
    CHECK_RETURN_IF_FALSE(result.ok())
        << "Failed to register SFP event callback.";
  }

  return next_id;
}

::util::Status OnlpPhal::UnregisterTransceiverEventWriter(int id) {
  absl::WriterMutexLock l(&config_lock_);
  if (!initialized_) {
    return MAKE_ERROR(ERR_NOT_INITIALIZED) << "Not initialized!";
  }

  auto it = std::find_if(
      transceiver_event_writers_.begin(), transceiver_event_writers_.end(),
      [id](const TransceiverEventWriter& h) { return h.id == id; });
  CHECK_RETURN_IF_FALSE(it != transceiver_event_writers_.end())
      << "Could not find a transceiver event Writer with ID " << id << ".";

  transceiver_event_writers_.erase(it);

  // Unregister OnlpSfpEventCallback if no more writer registered
  if (transceiver_event_writers_.size() == 0 &&
      sfp_event_callback_ != nullptr) {
    ::util::Status result = onlp_event_handler_->UnregisterSfpEventCallback(
        sfp_event_callback_.get());
    CHECK_RETURN_IF_FALSE(result.ok())
        << "Failed to unregister SFP event callback.";
    sfp_event_callback_ = nullptr;
  }

  return ::util::OkStatus();
}

::util::Status OnlpPhal::GetFrontPanelPortInfo(
    int slot, int port, FrontPanelPortInfo* fp_port_info) {
  absl::WriterMutexLock l(&config_lock_);
  if (!initialized_) {
    return MAKE_ERROR(ERR_NOT_INITIALIZED) << "Not initialized!";
  }

  if (slot < 0 || port < 0)
    RETURN_ERROR(ERR_INVALID_PARAM) << "Invalid Slot/Port value. ";

  // Get slot port pair to lookup sfpdatasource.
  const std::pair<int, int>& slot_port_pair = std::make_pair(slot, port);
  auto configurator = slot_port_to_configurator_[slot_port_pair];
  // Check to make sure we've got a configurator for this slot/port
  if (configurator == nullptr) {
    RETURN_ERROR() << "card[" << slot << "]/port[" << port << "]: "
                   << "no configurator for this transceiver";
  }
  auto sfp_src = configurator->GetSfpDataSource();

  // Check if Sfp inserted
  if (sfp_src == nullptr)
    RETURN_ERROR() << "card[" << slot << "]/port[" << port
                   << "]: Sfp not inserted";

  // Update sfp datasource values.
  sfp_src->UpdateValuesUnsafelyWithoutCacheOrLock();

  ManagedAttribute* sfptype_attrib = sfp_src->GetSfpType();

  ManagedAttribute* hw_state_attrib = sfp_src->GetSfpHardwareState();
  ASSIGN_OR_RETURN(
      auto hw_state_val,
      hw_state_attrib
          ->ReadValue<const google::protobuf::EnumValueDescriptor*>());
  fp_port_info->set_hw_state(static_cast<HwState>(hw_state_val->index()));

  if (fp_port_info->hw_state() == HW_STATE_NOT_PRESENT) {
    return ::util::OkStatus();
  }

  ASSIGN_OR_RETURN(
      auto sfptype_value,
      sfptype_attrib
          ->ReadValue<const google::protobuf::EnumValueDescriptor*>());

  SfpType sfval = static_cast<SfpType>(sfptype_value->index());
  // Need to map SfpType to PhysicalPortType
  PhysicalPortType actual_val;
  switch (sfval) {
    case SFP_TYPE_SFP:
      actual_val = PHYSICAL_PORT_TYPE_SFP_CAGE;
      break;
    case SFP_TYPE_QSFP_PLUS:
    case SFP_TYPE_QSFP:
    case SFP_TYPE_QSFP28:
      actual_val = PHYSICAL_PORT_TYPE_QSFP_CAGE;
      break;
    default:
      RETURN_ERROR(ERR_INVALID_PARAM) << "Invalid sfptype. ";
  }
  fp_port_info->set_physical_port_type(actual_val);

  ManagedAttribute* mediatype_attrib = sfp_src->GetSfpMediaType();
  ASSIGN_OR_RETURN(
      auto mediatype_value,
      mediatype_attrib
          ->ReadValue<const google::protobuf::EnumValueDescriptor*>());

  MediaType mediat_val = static_cast<MediaType>(mediatype_value->index());
  fp_port_info->set_media_type(mediat_val);

  ManagedAttribute* vendor_attrib = sfp_src->GetSfpVendor();
  ASSIGN_OR_RETURN(auto vendor_value, vendor_attrib->ReadValue<std::string>());
  fp_port_info->set_vendor_name(vendor_value);

  ManagedAttribute* model_attrib = sfp_src->GetSfpModel();
  ASSIGN_OR_RETURN(auto model_value, model_attrib->ReadValue<std::string>());
  fp_port_info->set_part_number(model_value);

  ManagedAttribute* serial_attrib = sfp_src->GetSfpSerialNumber();
  ASSIGN_OR_RETURN(auto serial_value, serial_attrib->ReadValue<std::string>());
  fp_port_info->set_serial_number(serial_value);

  return ::util::OkStatus();
}

OnlpPhal* OnlpPhal::CreateSingleton() {
  absl::WriterMutexLock l(&init_lock_);

  if (!singleton_) {
    singleton_ = new OnlpPhal();
    singleton_->Initialize();
  }

  return singleton_;
}

::util::Status OnlpPhal::WriteTransceiverEvent(const TransceiverEvent& event) {
  absl::WriterMutexLock l(&config_lock_);
  if (!initialized_) {
    return MAKE_ERROR(ERR_NOT_INITIALIZED) << "Not initialized!";
  }

  for (auto it = transceiver_event_writers_.begin();
       it != transceiver_event_writers_.end(); ++it) {
    it->writer->Write(event, absl::InfiniteDuration());
  }

  return ::util::OkStatus();
}

::util::Status OnlpPhal::SetPortLedState(int slot, int port, int channel,
                                         LedColor color, LedState state) {
  // TODO(unknown): Implement this.
  return ::util::OkStatus();
}

::util::Status OnlpPhal::InitializeOnlpInterface() {
  // Create the OnlpInterface object
  ASSIGN_OR_RETURN(onlp_interface_, OnlpWrapper::Make());
  return ::util::OkStatus();
}

::util::Status OnlpPhal::InitializeOnlpEventHandler() {
  // Create the OnlpEventHandler object
  ASSIGN_OR_RETURN(onlp_event_handler_,
                   OnlpEventHandler::Make(onlp_interface_.get()));
  return ::util::OkStatus();
}

// Register the configurator so we can use later
::util::Status OnlpPhal::RegisterSfpConfigurator(
    int slot, int port, SfpConfigurator* configurator) {
  const std::pair<int, int> slot_port_pair = std::make_pair(slot, port);

  slot_port_to_configurator_[slot_port_pair] =
      static_cast<OnlpSfpConfigurator*>(configurator);

  return ::util::OkStatus();
}

}  // namespace onlp
}  // namespace phal
}  // namespace hal
}  // namespace stratum

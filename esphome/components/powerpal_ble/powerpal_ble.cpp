#include "powerpal_ble.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"

#ifdef USE_ESP32

namespace esphome {
namespace powerpal_ble {

static const char *const TAG = "powerpal_ble";

void Powerpal::dump_config() {
  ESP_LOGCONFIG(TAG, "POWERPAL");
  LOG_SENSOR(" ", "Battery", this->battery_);
  LOG_SENSOR(" ", "Power", this->power_sensor_);
  LOG_SENSOR(" ", "Daily Energy", this->daily_energy_sensor_);
  LOG_SENSOR(" ", "Total Energy", this->energy_sensor_);
}

void Powerpal::setup() {
  this->authenticated_ = false;
  this->pulse_multiplier_ = ((seconds_in_minute * this->reading_batch_size_[0]) / (this->pulses_per_kwh_ / kw_to_w_conversion));
  ESP_LOGD(TAG, "pulse_multiplier_: %f", this->pulse_multiplier_ );

#ifdef USE_HTTP_REQUEST
    this->stored_measurements_.resize(15); //TODO dynamic
    this->cloud_uploader_->set_method("POST");
#endif
}

// void Powerpal::loop() {
//   // for (uint16_t i = 0; i < 15; i++) {
//   //   uint32_t timestamp = 1632487923494;
//   //   this->store_measurement_(i, timestamp+i);
//   // }
//   // this->upload_data_to_cloud_();

//   if (this->stored_measurements_.size()) {
//     uint32_t timestamp = 1632487923494;
//     this->store_measurement_(
//         this->stored_measurements_count_,
//         timestamp + this->stored_measurements_count_,
//         (uint32_t)roundf(this->stored_measurements_count_ * (this->pulses_per_kwh_ / kw_to_w_conversion)),
//         (this->stored_measurements_count_ / this->pulses_per_kwh_) * this->energy_cost_
//       );
//     if (this->stored_measurements_count_ == 14) {
//       this->upload_data_to_cloud_();
//     }
//   }
// }

std::string Powerpal::pkt_to_hex_(const uint8_t *data, uint16_t len) {
  char buf[64];
  memset(buf, 0, 64);
  for (int i = 0; i < len; i++)
    sprintf(&buf[i * 2], "%02x", data[i]);
  std::string ret = buf;
  return ret;
}

void Powerpal::decode_(const uint8_t *data, uint16_t length) {
  ESP_LOGD(TAG, "DEC(%d): 0x%s", length, this->pkt_to_hex_(data, length).c_str());
}

void Powerpal::parse_battery_(const uint8_t *data, uint16_t length) {
  ESP_LOGD(TAG, "Battery: DEC(%d): 0x%s", length, this->pkt_to_hex_(data, length).c_str());
  if (length == 1) {
    this->battery_->publish_state(data[0]);
  }
}

void Powerpal::parse_measurement_(const uint8_t *data, uint16_t length) {
  ESP_LOGD(TAG, "Meaurement: DEC(%d): 0x%s", length, this->pkt_to_hex_(data, length).c_str());
  if (length >= 6) {
    time_t unix_time = data[0];
    unix_time += (data[1] << 8);
    unix_time += (data[2] << 16);
    unix_time += (data[3] << 24);

    uint16_t pulses_within_interval = data[4];
    pulses_within_interval += data[5] << 8;

    // float total_kwh_within_interval = pulses_within_interval / this->pulses_per_kwh_;
    float avg_watts_within_interval = pulses_within_interval * this->pulse_multiplier_;

    ESP_LOGI(TAG, "Timestamp: %ld, Pulses: %d, Average Watts within interval: %f W", unix_time, pulses_within_interval,
             avg_watts_within_interval);

    if (this->power_sensor_ != nullptr) {
      this->power_sensor_->publish_state(avg_watts_within_interval);
    }

    if (this->energy_sensor_ != nullptr) {
      this->total_pulses_ += pulses_within_interval;
      float energy = this->total_pulses_ / this->pulses_per_kwh_;
      this->energy_sensor_->publish_state(energy);
    }

    if (this->daily_energy_sensor_ != nullptr) {
      // even if new day, publish last measurement window before resetting
      this->daily_pulses_ += pulses_within_interval;
      float energy = this->daily_pulses_ / this->pulses_per_kwh_;
      this->daily_energy_sensor_->publish_state(energy);

      // if esphome device has a valid time component set up, use that (preferred)
      // else, use the powerpal measurement timestamps
#ifdef USE_TIME
      auto *time_ = *this->time_;
      ESPTime date_of_measurement = time_->now();
      if (date_of_measurement.is_valid()) {
        if (this->day_of_last_measurement_ == 0) { this->day_of_last_measurement_ = date_of_measurement.day_of_year;}
        else if (this->day_of_last_measurement_ != date_of_measurement.day_of_year) {
          this->daily_pulses_ = 0;
          this->day_of_last_measurement_ = date_of_measurement.day_of_year;
        }
      } else {
        // if !date_of_measurement.is_valid(), user may have a bare "time:" in their yaml without a specific platform selected, so fallback to date of powerpal measurement
#else
        // avoid using ESPTime here so we don't need a time component in the config
        struct tm *date_of_measurement = ::localtime(&unix_time);
        // date_of_measurement.tm_yday + 1 because we are matching ESPTime day of year (1-366 instead of 0-365), which lets us catch a day_of_last_measurement_ of 0 as uninitialised
        if (this->day_of_last_measurement_ == 0) { this->day_of_last_measurement_ = date_of_measurement->tm_yday + 1 ;}
        else if (this->day_of_last_measurement_ != date_of_measurement->tm_yday + 1) {
          this->daily_pulses_ = 0;
          this->day_of_last_measurement_ = date_of_measurement->tm_yday + 1;
        }
#endif
#ifdef USE_TIME
      }
#endif
    }

#ifdef USE_HTTP_REQUEST
    if(this->cloud_uploader_ != nullptr) {
      this->store_measurement_(
        pulses_within_interval,
        unix_time,
        (uint32_t)roundf(pulses_within_interval * (this->pulses_per_kwh_ / kw_to_w_conversion)),
        (pulses_within_interval / this->pulses_per_kwh_) * this->energy_cost_
      );
      if (this->stored_measurements_count_ == 14) {
        this->upload_data_to_cloud_();
      }
    }
#endif
  }
}

std::string Powerpal::uuid_to_device_id_(const uint8_t *data, uint16_t length) {
  const char* hexmap[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "a", "b", "c", "d", "e", "f"};
  std::string device_id;
  for (int i = length-1; i >= 0; i--) {
    device_id.append(hexmap[(data[i] & 0xF0) >> 4]);
    device_id.append(hexmap[data[i] & 0x0F]);
  }
  return device_id;
}

std::string Powerpal::serial_to_apikey_(const uint8_t *data, uint16_t length) {
  const char* hexmap[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "a", "b", "c", "d", "e", "f"};
  std::string api_key;
  for (int i = 0; i < length; i++) {
    if ( i == 4 || i == 6 || i == 8 || i == 10 ) {
      api_key.append("-");
    }
    api_key.append(hexmap[(data[i] & 0xF0) >> 4]);
    api_key.append(hexmap[data[i] & 0x0F]);
  }
  return api_key;
}

#ifdef USE_HTTP_REQUEST
void Powerpal::store_measurement_(uint16_t pulses, time_t timestamp, uint32_t watt_hours, float cost) {
  this->stored_measurements_count_++;
  this->stored_measurements_[this->stored_measurements_count_].pulses = pulses;
  this->stored_measurements_[this->stored_measurements_count_].timestamp = timestamp;
  this->stored_measurements_[this->stored_measurements_count_].watt_hours = watt_hours;
  this->stored_measurements_[this->stored_measurements_count_].cost = cost;
}

void Powerpal::upload_data_to_cloud_() {
  this->stored_measurements_count_ = 0;
  if (this->powerpal_device_id_.length() && this->powerpal_apikey_.length()) {
    StaticJsonDocument<2048> doc; // 768 bytes, each entry may take up 15 bytes (uint16_t + uint32_t + uint32_t + float + bool)
    JsonArray array = doc.to<JsonArray>();
    for (int i = 0; i < 15; i++) {
      JsonObject nested = array.createNestedObject();
      nested["timestamp"] = this->stored_measurements_[i].timestamp;
      nested["pulses"] = this->stored_measurements_[i].pulses;
      nested["watt_hours"] = this->stored_measurements_[i].watt_hours;
      nested["cost"] = this->stored_measurements_[i].cost;
      nested["is_peak"] = false;
    }
    std::string body;
    serializeJson(doc, body);
    this->cloud_uploader_->set_body(body);
    // empty triggers, but requirement of using the send function
    std::vector<http_request::HttpRequestResponseTrigger *> response_triggers_;
    this->cloud_uploader_->send(response_triggers_);
  } else {
    // apikey or device missing
  }
}
#endif

void Powerpal::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                   esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_DISCONNECT_EVT: {
      this->authenticated_ = false;
      break;
    }
    case ESP_GATTC_SEARCH_CMPL_EVT: {
      // auto *pairing_code_char_ = this->parent_->get_characteristic(POWERPAL_SERVICE_UUID,
      // POWERPAL_CHARACTERISTIC_PAIRING_CODE_UUID); if (pairing_code_char_ == nullptr) {
      //   ESP_LOGE(TAG, "[%s] No Powerpal service or Pairing Code Characteristic found at device, not a POWERPAL..?",
      //             this->parent_->address_str().c_str());
      //   break;
      // } else {
      //   this->pairing_code_char_handle_ = pairing_code_char_->handle;
      // }

      // auto *reading_batch_size_char_ = this->parent_->get_characteristic(POWERPAL_SERVICE_UUID,
      // POWERPAL_CHARACTERISTIC_READING_BATCH_SIZE_UUID); if (reading_batch_size_char_ == nullptr) {
      //   ESP_LOGE(TAG, "[%s] No Powerpal service or Reading Batch Size Characteristic found at device, not a
      //   POWERPAL..?",
      //             this->parent_->address_str().c_str());
      //   break;
      // } else {
      //   this->reading_batch_size_char_handle_ = reading_batch_size_char_->handle;
      // }

      // auto *measurement_char_ = this->parent_->get_characteristic(POWERPAL_SERVICE_UUID,
      // POWERPAL_CHARACTERISTIC_MEASUREMENT_UUID); if (measurement_char_ == nullptr) {
      //   ESP_LOGE(TAG, "[%s] No Powerpal service or Measurement Characteristic found at device, not a POWERPAL..?",
      //             this->parent_->address_str().c_str());
      //   break;
      // } else {
      //   this->measurement_char_handle_ = measurement_char_->handle;
      // }

      // auto *uuid_char_ = this->parent_->get_characteristic(POWERPAL_SERVICE_UUID,
      // POWERPAL_CHARACTERISTIC_UUID_UUID); if (uuid_char_ == nullptr) {
      //   ESP_LOGE(TAG, "[%s] No Powerpal service or Measurement Characteristic found at device, not a POWERPAL..?",
      //             this->parent_->address_str().c_str());
      //   break;
      // } else {
      //   this->uuid_char_handle_ = uuid_char_->handle;
      //   ESP_LOGE(TAG, "UUID HANDLE: %d",this->uuid_char_handle_);
      // }

      // auto *serial_char_ = this->parent_->get_characteristic(POWERPAL_SERVICE_UUID,
      // POWERPAL_CHARACTERISTIC_SERIAL_UUID); if (serial_char_ == nullptr) {
      //   ESP_LOGE(TAG, "[%s] No Powerpal service or Measurement Characteristic found at device, not a POWERPAL..?",
      //             this->parent_->address_str().c_str());
      //   break;
      // } else {
      //   this->serial_number_char_handle_ = serial_char_->handle;
      //   ESP_LOGE(TAG, "SERIAL HANDLE: %d",this->serial_number_char_handle_);
      // }

      break;
    }
    case ESP_GATTC_READ_CHAR_EVT: {
      ESP_LOGD(TAG, "[%s] ESP_GATTC_READ_CHAR_EVT (Received READ)", this->parent_->address_str().c_str());
      if (param->read.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "Error reading char at handle %d, status=%d", param->read.handle, param->read.status);
        break;
      }
      // reading batch size
      if (param->read.handle == this->reading_batch_size_char_handle_) {
        ESP_LOGD(TAG, "Recieved reading_batch_size read event");
        this->decode_(param->read.value, param->read.value_len);
        if (param->read.value_len == 4) {
          if (param->read.value[0] != this->reading_batch_size_[0]) {
            // reading batch size needs changing, so write
            auto status =
                esp_ble_gattc_write_char(this->parent()->gattc_if, this->parent()->conn_id,
                                         this->reading_batch_size_char_handle_, sizeof(this->reading_batch_size_),
                                         this->reading_batch_size_, ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
            if (status) {
              ESP_LOGW(TAG, "Error sending write request for batch_size, status=%d", status);
            }
          } else {
            // reading batch size is set correctly so subscribe to measurement notifications
            auto status = esp_ble_gattc_register_for_notify(this->parent_->gattc_if, this->parent_->remote_bda,
                                                            this->measurement_char_handle_);
            if (status) {
              ESP_LOGW(TAG, "[%s] esp_ble_gattc_register_for_notify failed, status=%d",
                       this->parent_->address_str().c_str(), status);
            }
          }
        } else {
          // error, length should be 4
        }
        break;
      }

      // battery
      if (param->read.handle == this->battery_char_handle_) {
        ESP_LOGD(TAG, "Recieved battery read event");
        this->parse_battery_(param->read.value, param->read.value_len);
        break;
      }

      // firmware
      if (param->read.handle == this->firmware_char_handle_) {
        ESP_LOGD(TAG, "Recieved firmware read event");
        this->decode_(param->read.value, param->read.value_len);
        break;
      }

      // led sensitivity
      if (param->read.handle == this->led_sensitivity_char_handle_) {
        ESP_LOGD(TAG, "Recieved led sensitivity read event");
        this->decode_(param->read.value, param->read.value_len);
        break;
      }

      // serialNumber
      if (param->read.handle == this->serial_number_char_handle_) {
        ESP_LOGI(TAG, "Recieved uuid read event");
        this->powerpal_device_id_ = this->uuid_to_device_id_(param->read.value, param->read.value_len);
        ESP_LOGI(TAG, "Powerpal device id: %s", this->powerpal_device_id_.c_str());
#ifdef USE_HTTP_REQUEST
        this->powerpal_api_root_.append(this->powerpal_device_id_);
        this->cloud_uploader_->set_url(this->powerpal_api_root_);
#endif
        break;
      }

      // uuid
      if (param->read.handle == this->uuid_char_handle_) {
        ESP_LOGI(TAG, "Recieved serial_number read event");
        this->powerpal_apikey_ = this->serial_to_apikey_(param->read.value, param->read.value_len);
        ESP_LOGI(TAG, "Powerpal apikey: %s", this->powerpal_apikey_.c_str());
#ifdef USE_HTTP_REQUEST
        http_request::Header acceptheader;
        acceptheader.name = "Accept";
        acceptheader.value = "application/json";
        http_request::Header contentheader;
        contentheader.name = "Content-Type";
        contentheader.value = "application/json";
        http_request::Header authheader;
        authheader.name = "Authorization";
        authheader.value = this->powerpal_apikey_.c_str();
        std::list<http_request::Header> headers;
        headers.push_back(acceptheader);
        headers.push_back(contentheader);
        headers.push_back(authheader);
        this->cloud_uploader_->set_headers(headers);
#endif
        break;
      }

      break;
    }

    case ESP_GATTC_WRITE_CHAR_EVT: {
      ESP_LOGD(TAG, "[%s] ESP_GATTC_WRITE_CHAR_EVT (Write confirmed)", this->parent_->address_str().c_str());
      if (param->write.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "Error writing value to char at handle %d, status=%d", param->write.handle, param->write.status);
        break;
      }

      if (param->write.handle == this->pairing_code_char_handle_ && !this->authenticated_) {
        this->authenticated_ = true;

        auto read_reading_batch_size_status =
            esp_ble_gattc_read_char(this->parent()->gattc_if, this->parent()->conn_id,
                                    this->reading_batch_size_char_handle_, ESP_GATT_AUTH_REQ_NONE);
        if (read_reading_batch_size_status) {
          ESP_LOGW(TAG, "Error sending read request for reading batch size, status=%d", read_reading_batch_size_status);
        }

        if (!this->powerpal_apikey_.length()) {
          // read uuid (apikey)
          auto read_uuid_status = esp_ble_gattc_read_char(this->parent()->gattc_if, this->parent()->conn_id,
                                                            this->uuid_char_handle_, ESP_GATT_AUTH_REQ_NONE);
          if (read_uuid_status) {
            ESP_LOGW(TAG, "Error sending read request for powerpal uuid, status=%d", read_uuid_status);
          }
        }
        if (!this->powerpal_device_id_.length()) {
          // read serial number (device id)
          auto read_serial_number_status = esp_ble_gattc_read_char(this->parent()->gattc_if, this->parent()->conn_id,
                                                            this->serial_number_char_handle_, ESP_GATT_AUTH_REQ_NONE);
          if (read_serial_number_status) {
            ESP_LOGW(TAG, "Error sending read request for powerpal serial number, status=%d", read_serial_number_status);
          }
        }

        if (this->battery_ != nullptr) {
          // read battery
          auto read_battery_status = esp_ble_gattc_read_char(this->parent()->gattc_if, this->parent()->conn_id,
                                                             this->battery_char_handle_, ESP_GATT_AUTH_REQ_NONE);
          if (read_battery_status) {
            ESP_LOGW(TAG, "Error sending read request for battery, status=%d", read_battery_status);
          }
          // Enable notifications for battery
          auto notify_battery_status = esp_ble_gattc_register_for_notify(
              this->parent_->gattc_if, this->parent_->remote_bda, this->battery_char_handle_);
          if (notify_battery_status) {
            ESP_LOGW(TAG, "[%s] esp_ble_gattc_register_for_notify failed, status=%d",
                     this->parent_->address_str().c_str(), notify_battery_status);
          }
        }

        // read firmware version
        auto read_firmware_status =
            esp_ble_gattc_read_char(this->parent()->gattc_if, this->parent()->conn_id,
                                    this->firmware_char_handle_, ESP_GATT_AUTH_REQ_NONE);
        if (read_firmware_status) {
          ESP_LOGW(TAG, "Error sending read request for led sensitivity, status=%d", read_firmware_status);
        }

        // read led sensitivity
        auto read_led_sensitivity_status =
            esp_ble_gattc_read_char(this->parent()->gattc_if, this->parent()->conn_id,
                                    this->led_sensitivity_char_handle_, ESP_GATT_AUTH_REQ_NONE);
        if (read_led_sensitivity_status) {
          ESP_LOGW(TAG, "Error sending read request for led sensitivity, status=%d", read_led_sensitivity_status);
        }

        break;
      }
      if (param->write.handle == this->reading_batch_size_char_handle_) {
        // reading batch size is now set correctly so subscribe to measurement notifications
        auto status = esp_ble_gattc_register_for_notify(this->parent_->gattc_if, this->parent_->remote_bda,
                                                        this->measurement_char_handle_);
        if (status) {
          ESP_LOGW(TAG, "[%s] esp_ble_gattc_register_for_notify failed, status=%d",
                   this->parent_->address_str().c_str(), status);
        }
        break;
      }

      ESP_LOGW(TAG, "[%s] Missed all handle matches: %d",
               this->parent_->address_str().c_str(), param->write.handle);
      break;
    }  // ESP_GATTC_WRITE_CHAR_EVT

    case ESP_GATTC_NOTIFY_EVT: {
      ESP_LOGD(TAG, "[%s] Received Notification", this->parent_->address_str().c_str());

      // battery
      if (param->notify.handle == this->battery_char_handle_) {
        ESP_LOGD(TAG, "Recieved battery notify event");
        this->parse_battery_(param->notify.value, param->notify.value_len);
        break;
      }

      // measurement
      if (param->notify.handle == this->measurement_char_handle_) {
        ESP_LOGD(TAG, "Recieved measurement notify event");
        this->parse_measurement_(param->notify.value, param->notify.value_len);
        break;
      }
      break;  // registerForNotify
    }
    default:
      break;
  }
}

void Powerpal::gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  switch (event) {
    // This event is sent once authentication has completed
    case ESP_GAP_BLE_AUTH_CMPL_EVT: {
      if (param->ble_security.auth_cmpl.success) {
        ESP_LOGI(TAG, "[%s] Writing pairing code to Powerpal", this->parent_->address_str().c_str());
        auto status = esp_ble_gattc_write_char(this->parent()->gattc_if, this->parent()->conn_id,
                                               this->pairing_code_char_handle_, sizeof(this->pairing_code_),
                                               this->pairing_code_, ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
        if (status) {
          ESP_LOGW(TAG, "Error sending write request for pairing_code, status=%d", status);
        }
      }
      break;
    }
    default:
      break;
  }
}

}  // namespace powerpal_ble
}  // namespace esphome

#endif

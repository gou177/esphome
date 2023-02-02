// ENS160 sensor with I2C interface from ScioSense
//
// Datasheet: https://www.sciosense.com/wp-content/uploads/documents/SC-001224-DS-7-ENS160-Datasheet.pdf
//
// Implementation based on:
//   https://github.com/sciosense/ENS160_driver

#include "ens160.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"

namespace esphome {
namespace ens160 {

static const char *const TAG = "ens160";

static const uint16_t ENS160_PARTID = 0x6001;
static const uint8_t ENS160_BOOTING = 10;

static const uint8_t ENS160_REG_PART_ID = 0x00;
static const uint8_t ENS160_REG_OPMODE = 0x10;
static const uint8_t ENS160_REG_CONFIG = 0x11;
static const uint8_t ENS160_REG_COMMAND = 0x12;
static const uint8_t ENS160_REG_TEMP_IN = 0x13;
static const uint8_t ENS160_REG_RH_IN = 0x15;
static const uint8_t ENS160_REG_DATA_STATUS = 0x20;
static const uint8_t ENS160_REG_DATA_AQI = 0x21;
static const uint8_t ENS160_REG_DATA_TVOC = 0x22;
static const uint8_t ENS160_REG_DATA_ECO2 = 0x24;
static const uint8_t ENS160_REG_DATA_BL = 0x28;
static const uint8_t ENS160_REG_DATA_T = 0x30;
static const uint8_t ENS160_REG_DATA_RH = 0x32;
static const uint8_t ENS160_REG_DATA_MISR = 0x38;
static const uint8_t ENS160_REG_GPR_WRITE_0 = 0x40;
static const uint8_t ENS160_REG_GPR_WRITE_1 = ENS160_REG_GPR_WRITE_0 + 1;
static const uint8_t ENS160_REG_GPR_WRITE_2 = ENS160_REG_GPR_WRITE_0 + 2;
static const uint8_t ENS160_REG_GPR_WRITE_3 = ENS160_REG_GPR_WRITE_0 + 3;
static const uint8_t ENS160_REG_GPR_WRITE_4 = ENS160_REG_GPR_WRITE_0 + 4;
static const uint8_t ENS160_REG_GPR_WRITE_5 = ENS160_REG_GPR_WRITE_0 + 5;
static const uint8_t ENS160_REG_GPR_WRITE_6 = ENS160_REG_GPR_WRITE_0 + 6;
static const uint8_t ENS160_REG_GPR_WRITE_7 = ENS160_REG_GPR_WRITE_0 + 7;
static const uint8_t ENS160_REG_GPR_READ_0 = 0x48;
static const uint8_t ENS160_REG_GPR_READ_4 = ENS160_REG_GPR_READ_0 + 4;
static const uint8_t ENS160_REG_GPR_READ_6 = ENS160_REG_GPR_READ_0 + 6;
static const uint8_t ENS160_REG_GPR_READ_7 = ENS160_REG_GPR_READ_0 + 7;

static const uint8_t ENS160_COMMAND_NOP = 0x00;
static const uint8_t ENS160_COMMAND_CLRGPR = 0xCC;
static const uint8_t ENS160_COMMAND_GET_APPVER = 0x0E;

static const uint8_t ENS160_OPMODE_RESET = 0xF0;
static const uint8_t ENS160_OPMODE_IDLE = 0x01;
static const uint8_t ENS160_OPMODE_STD = 0x02;

static const uint8_t ENS160_DATA_STATUS_STATER = 0x07;
static const uint8_t ENS160_DATA_STATUS_NEWDAT = 0x02;
static const uint8_t ENS160_DATA_STATUS_NEWGPR = 0x01;

#define IS_NEWDAT(x) (ENS160_DATA_STATUS_NEWDAT == (ENS160_DATA_STATUS_NEWDAT & (x)))
#define IS_NEWGPR(x) (ENS160_DATA_STATUS_NEWGPR == (ENS160_DATA_STATUS_NEWGPR & (x)))
#define IS_NEW_DATA_AVAILABLE(x) (0 != ((ENS160_DATA_STATUS_NEWDAT | ENS160_DATA_STATUS_NEWGPR) & (x)))

#define CHECK_TRUE(f, error_code) \
  if (!(f)) { \
    this->mark_failed(); \
    this->error_code_ = (error_code); \
    return; \
  }

#define CHECKED_IO(f) CHECK_TRUE(f, COMMUNICATION_FAILED)

static const uint16_t ENS160_PART_ID = 0x0160;

void ENS160Component::setup() {
  ESP_LOGCONFIG(TAG, "Setting up ENS160...");

  // set mode to reset
  CHECKED_IO(this->write_byte(ENS160_REG_OPMODE, ENS160_OPMODE_RESET));
  delay(ENS160_BOOTING);

  // check part_id
  uint16_t part_id;
  CHECKED_IO(this->read_byte_16(ENS160_REG_PART_ID, &part_id))
  ESP_LOGD(TAG, "Setup Part ID: %x\n", part_id);
  CHECK_TRUE(part_id == ENS160_PARTID, INVALID_ID);

  // set mode to idle
  CHECKED_IO(this->write_byte(ENS160_REG_OPMODE, ENS160_OPMODE_IDLE));

  // clear command
  CHECKED_IO(this->write_byte(ENS160_REG_COMMAND, ENS160_COMMAND_NOP));
  CHECKED_IO(this->write_byte(ENS160_REG_COMMAND, ENS160_COMMAND_CLRGPR));

  // get status
  ESP_LOGD(TAG, "Setup Done. Status: %x\n", this->read_status_().value_or(0));

  // read firmware version
  CHECKED_IO(this->write_byte(ENS160_REG_COMMAND, ENS160_COMMAND_GET_APPVER));
  auto version_data = this->read_bytes<3>(ENS160_REG_GPR_READ_4);
  if (version_data.has_value()) {
    uint8_t fw_ver_major = (*version_data)[0];
    uint8_t fw_ver_minor = (*version_data)[1];
    uint8_t fw_ver_build = (*version_data)[2];
    if (this->version_ != nullptr) {
      char version[32];
      sprintf(version, "%d.%d.%d", fw_ver_major, fw_ver_minor, fw_ver_build);
      ESP_LOGD(TAG, "publishing version state: %s", version);
      this->version_->publish_state(version);
    }
  }

  // set mode to standard
  CHECKED_IO(this->write_byte(ENS160_REG_OPMODE, ENS160_OPMODE_STD));
  delay(ENS160_BOOTING);
}

optional<uint8_t> ENS160Component::read_status_() { return this->read_byte(ENS160_REG_DATA_STATUS); }

bool ENS160Component::status_has_error_() {
  optional<uint8_t> status = this->read_status_();
  if (!status.has_value())
    return true;
  return ENS160_DATA_STATUS_STATER == (ENS160_DATA_STATUS_STATER & (status.value()));
}

bool ENS160Component::status_has_data_() {
  optional<uint8_t> status = this->read_status_();
  if (!status.has_value()) return false;
  return IS_NEW_DATA_AVAILABLE(status.value());
}

void ENS160Component::update() {
  optional<uint8_t> status = this->read_status_();
  if (status.has_value()) {
    ESP_LOGD(TAG, "Status: %x", status.value());
  } else {
    ESP_LOGD(TAG, "Status: no data");
    return;
  }

  if (!this->status_has_data_()) {
    ESP_LOGD(TAG, "Status indicates no data ready!");
    this->status_set_error();
    return;
  }

	if (IS_NEWGPR(status.value())) {
		this->read_byte(ENS160_REG_GPR_READ_0);
  }

  if (!IS_NEWDAT(status.value())) {
    return;
  }

  auto buf_eco2 = this->read_bytes<2>(ENS160_REG_DATA_ECO2);
  auto buf_tvoc = this->read_bytes<2>(ENS160_REG_DATA_TVOC);
  auto data_aqi = this->read_byte(ENS160_REG_DATA_AQI);

  if (buf_eco2.has_value() && this->co2_ != nullptr) {
    uint16_t data_eco2 = encode_uint16((*buf_eco2)[1], (*buf_eco2)[0]);
    this->co2_->publish_state(data_eco2);
  } else {
    ESP_LOGW(TAG, "No data for eCO2!");
  }
  if (buf_tvoc.has_value() && this->tvoc_ != nullptr) {
    uint16_t data_tvoc = encode_uint16((*buf_tvoc)[1], (*buf_tvoc)[0]);
    this->tvoc_->publish_state(data_tvoc);
  } else {
    ESP_LOGW(TAG, "No data for TVOC!");
  }
  if (data_aqi.has_value() && this->aqi_ != nullptr) {
    this->aqi_->publish_state(*data_aqi);
  } else {
    ESP_LOGW(TAG, "No data for AQI!");
  }

  this->status_clear_warning();
  this->send_env_data_();
}

void ENS160Component::send_env_data_() {
  if (this->humidity_ == nullptr && this->temperature_ == nullptr)
    return;

  float humidity = NAN;
  if (this->humidity_ != nullptr)
    humidity = this->humidity_->state;

  float temperature = NAN;
  if (this->temperature_ != nullptr)
    temperature = this->temperature_->state;

  uint16_t t = (uint16_t)((temperature + 273.15f) * 64.0f);
  uint16_t h = (uint16_t)(humidity * 512.0f);

  uint8_t data[4];
  data[0] = t & 0xff;
  data[1] = (t >> 8) & 0xff;
  data[2] = h & 0xff;
  data[3] = (h >> 8) & 0xff;

  CHECKED_IO(this->write_bytes(ENS160_REG_TEMP_IN, data, 4));
}

void ENS160Component::dump_config() {
  ESP_LOGCONFIG(TAG, "ENS160");
  LOG_I2C_DEVICE(this)
  LOG_UPDATE_INTERVAL(this)
  LOG_SENSOR("  ", "CO2 Sensor", this->co2_)
  LOG_SENSOR("  ", "TVOC Sensor", this->tvoc_)
  LOG_SENSOR("  ", "AQI Sensor", this->aqi_)
  // LOG_TEXT_SENSOR("  ", "Firmware Version Sensor", this->version_)
  if (this->is_failed()) {
    switch (this->error_code_) {
      case COMMUNICATION_FAILED:
        ESP_LOGW(TAG, "Communication failed! Is the sensor connected?");
        break;
      case INVALID_ID:
        ESP_LOGW(TAG, "Sensor reported an invalid ID. Is this a ENS160?");
        break;
      case SENSOR_REPORTED_ERROR:
        ESP_LOGW(TAG, "Sensor reported internal error");
        break;
      case UNKNOWN:
      default:
        ESP_LOGW(TAG, "Unknown setup error!");
        break;
    }
  }
}

}  // namespace ens160
}  // namespace esphome

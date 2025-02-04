#pragma once
#include <stdlib.h>

// Supported device types.
enum dk_device_type {
    DK_MODBUS_TCP,
    DK_MODBUS_RTU,
    DK_AB_EIP,
    DK_S7,
};

enum dk_modbus_value_type {
    DK_MODBUS_REAL,
    DK_MODBUS_INT,
    DK_MODBUS_COIL,
};

enum dk_modbus_function {
    DK_READ_COILS = 1,
    DK_READ_DISCRETE_INPUTS,
    DK_READ_HOLDING_REGISTERS,
    DK_READ_INPUT_REGISTERS,
};
struct dk_modbus_tag {
    char *name;
    uint16_t address;
    enum dk_modbus_value_type value_type;
    enum dk_modbus_function modbus_function;
};

struct dk_modbus_tcp_dv {
    char *name;
    char *ip;
    uint16_t port;
    uint32_t timeout;
    uint32_t poll_delay;
    uint32_t tags_count;
    struct dk_modbus_tag *tags;
};

struct dk_config {
    struct dk_modbus_tcp_dv *modbus_tcp_devices;
    uint16_t modbus_tcp_devices_count;
    char *db_path;
};

#include "diskette.h"
#include "sds.h"
#include "sqlite3.h"
#include <cyaml/cyaml.h>
#include <errno.h>
#include <modbus/modbus.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/_pthread/_pthread_mutex_t.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

//#include <pthread.h>

/******************************************************************************
 * C data structure for storing a project plan.
 *
 * This is what we want to load the YAML into.
 ******************************************************************************/

/******************************************************************************
 * CYAML schema to tell libcyaml about both expected YAML and data structure.
 *
 * (Our CYAML schema is just a bunch of static const data.)
 ******************************************************************************/

static const cyaml_strval_t dk_modbus_function_strings[] = {
    {"Coil", DK_READ_COILS},
    {"Discrete", DK_READ_DISCRETE_INPUTS},
    {"Holding", DK_READ_HOLDING_REGISTERS},
    {"Input", DK_READ_INPUT_REGISTERS},
};

static const cyaml_strval_t dk_modbus_value_strings[] = {
    {"Real", DK_MODBUS_REAL},
    {"Int", DK_MODBUS_INT},
    {"Coil", DK_MODBUS_COIL},
};
/* Mapping from "task_flags" strings to enum values for schema. */

/* Schema for string pointer values (used in sequences of strings). */
static const cyaml_schema_value_t string_ptr_schema = {
    CYAML_VALUE_STRING(CYAML_FLAG_POINTER, char, 0, CYAML_UNLIMITED),
};

static const cyaml_schema_field_t dk_modbus_tag_fields_schema[] = {
    CYAML_FIELD_STRING_PTR("name", CYAML_FLAG_POINTER, struct dk_modbus_tag,
                           name, 0, CYAML_UNLIMITED),
    CYAML_FIELD_UINT("address", CYAML_FLAG_DEFAULT, struct dk_modbus_tag,
                     address),
    CYAML_FIELD_ENUM("value_type", CYAML_FLAG_DEFAULT, struct dk_modbus_tag,
                     value_type, dk_modbus_value_strings,
                     CYAML_ARRAY_LEN(dk_modbus_value_strings)),
    CYAML_FIELD_ENUM("modbus_function", CYAML_FLAG_DEFAULT,
                     struct dk_modbus_tag, modbus_function,
                     dk_modbus_function_strings,
                     CYAML_ARRAY_LEN(dk_modbus_function_strings)),

    CYAML_FIELD_END};

static const cyaml_schema_value_t dk_modbus_tag_schema = {
    CYAML_VALUE_MAPPING(CYAML_FLAG_DEFAULT, struct dk_modbus_tag,
                        dk_modbus_tag_fields_schema),
};

static const cyaml_schema_field_t dk_mb_tcp_dv_fields_schema[] = {
    CYAML_FIELD_STRING_PTR("name", CYAML_FLAG_POINTER, struct dk_modbus_tcp_dv,
                           name, 0, CYAML_UNLIMITED),
    CYAML_FIELD_STRING_PTR("ip", CYAML_FLAG_POINTER, struct dk_modbus_tcp_dv,
                           ip, 0, CYAML_UNLIMITED),
    CYAML_FIELD_UINT("port", CYAML_FLAG_DEFAULT, struct dk_modbus_tcp_dv, port),
    CYAML_FIELD_UINT("timeout", CYAML_FLAG_DEFAULT, struct dk_modbus_tcp_dv,
                     timeout),
    CYAML_FIELD_UINT("poll_delay", CYAML_FLAG_DEFAULT, struct dk_modbus_tcp_dv,
                     poll_delay),

    CYAML_FIELD_SEQUENCE("tags", CYAML_FLAG_POINTER, struct dk_modbus_tcp_dv,
                         tags, &dk_modbus_tag_schema, 0, CYAML_UNLIMITED),

    CYAML_FIELD_END};

static const cyaml_schema_value_t dk_mb_tcp_dv_schema = {
    CYAML_VALUE_MAPPING(CYAML_FLAG_DEFAULT, struct dk_modbus_tcp_dv,
                        dk_mb_tcp_dv_fields_schema),
};

static const cyaml_schema_field_t dk_config_fields_schema[] = {
    CYAML_FIELD_SEQUENCE("modbus_tcp_devices", CYAML_FLAG_POINTER,
                         struct dk_config, modbus_tcp_devices,
                         &dk_mb_tcp_dv_schema, 0, CYAML_UNLIMITED),
    CYAML_FIELD_STRING_PTR("db_path", CYAML_FLAG_POINTER, struct dk_config,
                           db_path, 0, CYAML_UNLIMITED),
    CYAML_FIELD_END};
static const cyaml_schema_value_t dk_config_schema = {
    CYAML_VALUE_MAPPING(CYAML_FLAG_POINTER, struct dk_config,
                        dk_config_fields_schema),
};

/******************************************************************************
 * Actual code to load and save YAML doc using libcyaml.
 ******************************************************************************/

/* Our CYAML config.
 *
 * If you want to change it between calls, don't make it const.
 *
 * Here we have a very basic config.
 */
static const cyaml_config_t config = {
    .log_fn = cyaml_log,            /* Use the default logging function. */
    .mem_fn = cyaml_mem,            /* Use the default memory allocator. */
    .log_level = CYAML_LOG_WARNING, /* Logging errors and warnings only. */
};

static void *dk_mb_tcp_thread(struct dk_modbus_tcp_dv *device) {

    // struct dk_modbus_tcp_dv *device = (struct dk_modbus_tcp_dv *)arg;
    modbus_t *mb_tcp_ctx;
    mb_tcp_ctx = modbus_new_tcp(device->ip, device->port);

    if (mb_tcp_ctx == NULL) {
        printf("Unable to allocate Modbus context\n");
    }

    if (modbus_connect(mb_tcp_ctx) == -1) {
        fprintf(stderr, "Cannot connect to Modbus server: %s\n",
                modbus_strerror(errno));
        modbus_free(mb_tcp_ctx);
    }

    for (;;) {
        sleep(device->poll_delay);
        printf("Polling device: %s\n", device->name);
        for (unsigned j = 0; j < device->tags_count; j++) {
            struct dk_modbus_tag tag = device->tags[j];
            printf("    %u. TAG_NAME: %s    FUN_CODE: %u     VALUE_TYPE: %u\n",
                   j + 1, tag.name, tag.modbus_function, tag.value_type);

            uint16_t buff[2];
            switch (tag.value_type) {

            case DK_MODBUS_INT:

                if (tag.modbus_function == DK_READ_COILS ||
                    tag.modbus_function == DK_READ_DISCRETE_INPUTS) {
                    printf("Modbus config error:\nTAG value type INT for "
                           "TAGNAME: %s in DEVICENAME: %s can only be "
                           "used with FUNC_CODE Read inputs or Read holdings\n",
                           tag.name, device->name);
                    break;
                }

                printf("Starting FUNC: %u for TAG type : %u\n",
                       tag.modbus_function, tag.value_type);

                if (tag.modbus_function == DK_READ_HOLDING_REGISTERS) {
                    if (modbus_read_registers(mb_tcp_ctx, tag.address, 1,
                                              buff) == -1) {
                        fprintf(stderr, "%s\n", modbus_strerror(errno));
                        break;
                    }

                    printf("MODBUS READ RESULT:\n    No    TAG    VALUE\n");
                    printf("    %u    %s    %d\n", j, tag.name, buff[0]);
                }

                if (tag.modbus_function == DK_READ_INPUT_REGISTERS) {
                    if (modbus_read_input_registers(mb_tcp_ctx, tag.address, 1,
                                                    buff) == -1) {
                        fprintf(stderr, "%s\n", modbus_strerror(errno));
                        break;
                    }

                    printf("MODBUS READ RESULT:\n    No    TAG    VALUE\n");
                    printf("    %u    %s    %d\n", j, tag.name, buff[0]);
                }

                break;

            case DK_MODBUS_REAL:

                if (tag.modbus_function == DK_READ_COILS ||
                    tag.modbus_function == DK_READ_DISCRETE_INPUTS) {
                    printf("Modbus config error:\nTAG value type REAL for "
                           "TAGNAME: %s in DEVICENAME: %s can only be "
                           "used with FUNC_CODE Read inputs or Read holdings\n",
                           tag.name, device->name);
                    break;
                }
                printf("Starting FUNC: %u for TAG type : %u\n",
                       tag.modbus_function, tag.value_type);

                if (tag.modbus_function == DK_READ_HOLDING_REGISTERS) {
                    // We read to registers at a time to be used for float.
                    if (modbus_read_registers(mb_tcp_ctx, tag.address, 2,
                                              buff) == -1) {
                        fprintf(stderr, "ERROR: %s\n", modbus_strerror(errno));
                        break;
                    }

                    // Get a float value from the two uint16 registers that we
                    // read into the buffer.
                    float res_float = modbus_get_float_abcd(buff);

                    printf("MODBUS READ RESULT:\n    No    TAG    VALUE"
                           "\n");
                    printf("    %u    %s    %f\n", j, tag.name, res_float);
                }

                if (tag.modbus_function == DK_READ_INPUT_REGISTERS) {
                    if (modbus_read_input_registers(mb_tcp_ctx, tag.address, 2,
                                                    buff) == -1) {
                        fprintf(stderr, "%s\n", modbus_strerror(errno));
                        break;
                    }

                    float res_float = modbus_get_float_dcba(buff);

                    printf("MODBUS READ RESULT:\n    No    TAG    VALUE[LSB]   "
                           " VALUE[MSB]\n");
                    printf("    %u    %s    %f\n", j, tag.name, res_float);
                }
                break;

            case DK_MODBUS_COIL:

                if (tag.modbus_function == DK_READ_HOLDING_REGISTERS ||
                    tag.modbus_function == DK_READ_INPUT_REGISTERS) {
                    printf("Modbus config error:\nTAG value type COIL for "
                           "TAGNAME: %s in DEVICENAME: %s can only be "
                           "used with FUNC_CODE Read coils or Read discrete "
                           "inputs.\n",
                           tag.name, device->name);
                    break;
                }
                printf("Starting FUNC: %u for TAG type : %u\n",
                       tag.modbus_function, tag.value_type);
                break;

            default:

                printf("Switch statment did not match.\n");
                break;
            };
        }
    }
}

// The global database context.
sqlite3 *db;
// The mutex protecting the database context.
// static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;

/* Main entry point from OS. */
int main(int argc, char *argv[]) {
    cyaml_err_t err;
    struct dk_config *dk_config;
    pthread_t t1;
    int pthread_ret;

    enum {
        ARG_PROG_NAME,
        ARG_PATH_IN,
        ARG__COUNT,
    };

    /* Handle args */
    if (argc != ARG__COUNT) {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  %s <INPUT>\n", argv[ARG_PROG_NAME]);
        return EXIT_FAILURE;
    }

    /* Load input file. */
    err = cyaml_load_file(argv[ARG_PATH_IN], &config, &dk_config_schema,
                          (void **)&dk_config, NULL);
    if (err != CYAML_OK) {
        fprintf(stderr, "ERROR: %s\n", cyaml_strerror(err));
        return EXIT_FAILURE;
    }

    int rc = sqlite3_open(dk_config->db_path, &db);
    if (rc != SQLITE_OK) {
        printf("Failed to open db.\n");
        return EXIT_FAILURE;
    }
    if (dk_config->modbus_tcp_devices_count != 1) {
        printf("Only one device can be configured.\n");
        return EXIT_FAILURE;
    }
    /* Use the data. */
    for (unsigned i = 0; i < dk_config->modbus_tcp_devices_count; i++) {

        struct dk_modbus_tcp_dv *device = &dk_config->modbus_tcp_devices[i];

        dk_mb_tcp_thread(device);

        // Spawn a new thread for each device and pass it a pointer to the
        // device struct data.
        // pthread_ret = pthread_create(&t1, NULL, dk_mb_tcp_thread, device);

        // if (pthread_ret != 0) {
        //     fprintf(stderr,
        //             "ERROR: Could not create thread for device No. %u\n",
        //             i + 1);
        //     return EXIT_FAILURE;
        // }

        // pthread_ret = pthread_join(t1, NULL);

        // if (pthread_ret != 0) {
        //     fprintf(stderr, "ERROR: Could not join thread for device No.
        //     %u\n",
        //             i + 1);
        //     return EXIT_FAILURE;
        // }
        // dk_mb_tcp_thread(device);
    }

    /* Free the data */
    cyaml_free(&config, &dk_config_schema, dk_config, 0);

    return EXIT_SUCCESS;
}

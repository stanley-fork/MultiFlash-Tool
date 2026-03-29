#include "mtk/mtk_service.h"
#include "mtk/mtk_port_detect.h"

#include <stdlib.h>
#include <string.h>

struct mtk_service {
    mtk_callbacks_t cb;
    mtk_transport_t* transport;
    mtk_da_session_t* da_session;
    mtk_device_info_t device_info;
    mtk_security_state_t security_state;
    mtk_capabilities_t capabilities;
    mtk_da_mode_t da_mode;
    int connected;
};

mtk_service_t* mtk_service_create(const mtk_callbacks_t* callbacks) {
    mtk_service_t* svc = (mtk_service_t*)calloc(1, sizeof(*svc));
    if (!svc) {
        return NULL;
    }
    if (callbacks) {
        svc->cb = *callbacks;
    }
    svc->transport = mtk_transport_create(callbacks);
    if (!svc->transport) {
        free(svc);
        return NULL;
    }
    return svc;
}

void mtk_service_destroy(mtk_service_t* svc) {
    if (!svc) {
        return;
    }
    mtk_service_disconnect(svc);
    mtk_transport_destroy(svc->transport);
    free(svc);
}

mtk_error_t mtk_service_connect(mtk_service_t* svc,
                                const mtk_connect_options_t* options) {
    mtk_error_t err;
    char auto_port[32] = {0};
    const char* port_name = NULL;
    if (!svc || !options) {
        return MTK_E_INVALID_ARG;
    }

    if (options->port_name && options->port_name[0]) {
        port_name = options->port_name;
    } else if (options->auto_detect_port) {
        err = mtk_port_detect_first(auto_port, sizeof(auto_port));
        if (err != MTK_OK) {
            return err;
        }
        port_name = auto_port;
    }

    if (!port_name || !port_name[0]) {
        return MTK_E_INVALID_ARG;
    }

    err = mtk_transport_open(svc->transport, port_name);
    if (err != MTK_OK) {
        return err;
    }

    err = mtk_boot_detect_stage(svc->transport, &svc->device_info);
    if (err != MTK_OK) {
        mtk_transport_close(svc->transport);
        return err;
    }

    err = mtk_security_probe(svc->transport, &svc->device_info, &svc->security_state, &svc->capabilities);
    if (err != MTK_OK) {
        mtk_transport_close(svc->transport);
        return err;
    }

    err = mtk_da_upload(svc->transport,
                        options,
                        &svc->device_info,
                        &svc->security_state,
                        &svc->da_session,
                        &svc->da_mode);
    if (err != MTK_OK) {
        mtk_transport_close(svc->transport);
        return err;
    }

    err = mtk_storage_probe(svc->da_session, &svc->device_info);
    if (err != MTK_OK) {
        mtk_da_close(svc->da_session);
        svc->da_session = NULL;
        mtk_transport_close(svc->transport);
        return err;
    }

    svc->device_info.boot_stage = MTK_BOOT_STAGE_DA;
    svc->security_state.can_read_flash = 1;
    svc->security_state.can_write_flash = 1;
    svc->security_state.can_erase_flash = 1;
    svc->capabilities.can_read_flash = 1;
    svc->capabilities.can_write_flash = 1;
    svc->capabilities.can_erase_flash = 1;
    svc->capabilities.can_read_partitions = 1;
    svc->capabilities.can_write_partitions = 1;
    svc->capabilities.can_erase_partitions = 1;
    svc->capabilities.can_reboot = 1;
    svc->connected = 1;
    return MTK_OK;
}

void mtk_service_disconnect(mtk_service_t* svc) {
    if (!svc) {
        return;
    }
    if (svc->da_session) {
        mtk_da_close(svc->da_session);
        svc->da_session = NULL;
    }
    if (svc->transport) {
        mtk_transport_close(svc->transport);
    }
    memset(&svc->device_info, 0, sizeof(svc->device_info));
    memset(&svc->security_state, 0, sizeof(svc->security_state));
    memset(&svc->capabilities, 0, sizeof(svc->capabilities));
    svc->da_mode = MTK_DA_MODE_UNKNOWN;
    svc->connected = 0;
}

int mtk_service_is_connected(const mtk_service_t* svc) {
    return svc ? svc->connected : 0;
}

mtk_error_t mtk_service_get_device_info(mtk_service_t* svc,
                                        mtk_device_info_t* out_info) {
    if (!svc || !out_info) {
        return MTK_E_INVALID_ARG;
    }
    if (!svc->connected) {
        return MTK_E_NOT_CONNECTED;
    }
    *out_info = svc->device_info;
    return MTK_OK;
}

mtk_error_t mtk_service_get_security_state(mtk_service_t* svc,
                                           mtk_security_state_t* out_state) {
    if (!svc || !out_state) {
        return MTK_E_INVALID_ARG;
    }
    if (!svc->connected) {
        return MTK_E_NOT_CONNECTED;
    }
    *out_state = svc->security_state;
    return MTK_OK;
}

mtk_error_t mtk_service_get_capabilities(mtk_service_t* svc,
                                         mtk_capabilities_t* out_caps) {
    if (!svc || !out_caps) {
        return MTK_E_INVALID_ARG;
    }
    if (!svc->connected) {
        return MTK_E_NOT_CONNECTED;
    }
    *out_caps = svc->capabilities;
    return MTK_OK;
}

mtk_error_t mtk_service_read_partition_table(mtk_service_t* svc,
                                             mtk_partition_info_t* parts,
                                             size_t* inout_count) {
    if (!svc) {
        return MTK_E_INVALID_ARG;
    }
    if (!svc->connected || !svc->da_session) {
        return MTK_E_NOT_CONNECTED;
    }
    return mtk_partition_read_table(svc->da_session, parts, inout_count);
}

mtk_error_t mtk_service_read_partition(mtk_service_t* svc,
                                       const mtk_partition_info_t* part,
                                       const char* out_path) {
    if (!svc || !part || !out_path) {
        return MTK_E_INVALID_ARG;
    }
    if (!svc->connected || !svc->da_session) {
        return MTK_E_NOT_CONNECTED;
    }
    return mtk_storage_read_blocks(svc->da_session,
                                   part->region,
                                   part->start_lba,
                                   part->lba_count,
                                   out_path);
}

mtk_error_t mtk_service_write_partition(mtk_service_t* svc,
                                        const mtk_partition_info_t* part,
                                        const char* image_path) {
    if (!svc || !part || !image_path) {
        return MTK_E_INVALID_ARG;
    }
    if (!svc->connected || !svc->da_session) {
        return MTK_E_NOT_CONNECTED;
    }
    return mtk_storage_write_blocks(svc->da_session,
                                    part->region,
                                    part->start_lba,
                                    part->lba_count,
                                    image_path);
}

mtk_error_t mtk_service_erase_partition(mtk_service_t* svc,
                                        const mtk_partition_info_t* part) {
    if (!svc || !part) {
        return MTK_E_INVALID_ARG;
    }
    if (!svc->connected || !svc->da_session) {
        return MTK_E_NOT_CONNECTED;
    }
    return mtk_storage_erase_blocks(svc->da_session,
                                    part->region,
                                    part->start_lba,
                                    part->lba_count);
}

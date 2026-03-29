#include "edl/fs_prop_probe.h"
#include "edl/sparse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void log_cb(const char *msg, void *user)
{
    (void)user;
    if (msg && msg[0])
        printf("%s\n", msg);
}

static void print_field(const char *label, const char *value)
{
    if (value && value[0])
        printf("%s: %s\n", label, value);
}

int main(int argc, char **argv)
{
    int argi = 1;
    int64_t max_bytes = (int64_t)512 * 1024 * 1024;

    if (argc >= 4 && strcmp(argv[1], "--max-mb") == 0) {
        max_bytes = (int64_t)atoll(argv[2]) * 1024 * 1024;
        argi = 3;
    }

    if (argc <= argi) {
        fprintf(stderr, "usage: %s [--max-mb N] <image> [image...]\n", argv[0]);
        return 2;
    }

    for (int i = argi; i < argc; i++) {
        const char *path = argv[i];
        uint8_t *buf = NULL;
        int size = 0;

        if (edl_sparse_is_sparse(path)) {
            edl_error_t err = edl_image_read_logical_prefix(path, &buf, &size,
                                                            max_bytes);
            if (err != EDL_OK || !buf || size <= 0) {
                fprintf(stderr, "sparse read failed: %s (%d)\n", path, (int)err);
                free(buf);
                return 1;
            }
        } else {
            FILE *fp = fopen(path, "rb");
            if (!fp) {
                fprintf(stderr, "open failed: %s\n", path);
                return 1;
            }
            if (fseek(fp, 0, SEEK_END) != 0) {
                fclose(fp);
                fprintf(stderr, "seek failed: %s\n", path);
                return 1;
            }
            long long file_size = _ftelli64(fp);
            if (file_size <= 0) {
                fclose(fp);
                fprintf(stderr, "size invalid: %s\n", path);
                return 1;
            }
            if (fseek(fp, 0, SEEK_SET) != 0) {
                fclose(fp);
                fprintf(stderr, "seek reset failed: %s\n", path);
                return 1;
            }

            if (file_size > 0x7fffffffLL) {
                fclose(fp);
                fprintf(stderr, "file too large for raw read: %s\n", path);
                return 1;
            }
            size = (int)file_size;
            buf = (uint8_t *)malloc((size_t)size);
            if (!buf) {
                fclose(fp);
                fprintf(stderr, "malloc failed: %s\n", path);
                return 1;
            }
            if (fread(buf, 1, (size_t)size, fp) != (size_t)size) {
                free(buf);
                fclose(fp);
                fprintf(stderr, "read failed: %s\n", path);
                return 1;
            }
            fclose(fp);
        }

        edl_android_props_t props;
        memset(&props, 0, sizeof(props));
        int64_t fs_off = 0;

        printf("==== %s ====\n", path);
        if (!edl_probe_android_props_from_buffer_scanned(buf,
                                                         size,
                                                         &props,
                                                         &fs_off,
                                                         true,
                                                         log_cb,
                                                         NULL)) {
            printf("probe: no props\n");
            free(buf);
            continue;
        }

        print_field("fs_type", props.fs_type);
        print_field("volume_label", props.volume_label);
        print_field("brand", props.brand);
        print_field("manufacturer", props.manufacturer);
        print_field("market_name", props.market_name);
        print_field("model", props.model);
        print_field("device", props.device);
        print_field("product", props.product);
        print_field("android_release", props.android_release);
        print_field("fingerprint", props.fingerprint);
        print_field("security_patch", props.security_patch);
        print_field("build_id", props.build_id);
        print_field("incremental", props.incremental);
        print_field("display_id", props.display_id);
        print_field("sdk", props.sdk);
        print_field("ota_version", props.ota_version);
        print_field("display_ota", props.display_ota);
        print_field("display_full_id", props.display_full_id);
        print_field("common_ota", props.common_ota);
        print_field("project_number", props.project_number);
        print_field("auth_project", props.auth_project);
        print_field("hardware_code", props.hardware_code);
        print_field("nv_id", props.nv_id);
        print_field("pipeline_key", props.pipeline_key);
        print_field("base_version", props.base_version);
        printf("fs_offset: 0x%llX\n", (unsigned long long)fs_off);
        free(buf);
    }

    return 0;
}

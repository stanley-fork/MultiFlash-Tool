#ifndef MTK_SERVICE_H
#define MTK_SERVICE_H

#include "mtk_partition.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mtk_service mtk_service_t;

MTK_API mtk_service_t* mtk_service_create(const mtk_callbacks_t* callbacks);
MTK_API void mtk_service_destroy(mtk_service_t* svc);

MTK_API mtk_error_t mtk_service_connect(mtk_service_t* svc,
                                        const mtk_connect_options_t* options);
MTK_API void mtk_service_disconnect(mtk_service_t* svc);
MTK_API int mtk_service_is_connected(const mtk_service_t* svc);

MTK_API mtk_error_t mtk_service_get_device_info(mtk_service_t* svc,
                                                mtk_device_info_t* out_info);
MTK_API mtk_error_t mtk_service_get_security_state(mtk_service_t* svc,
                                                   mtk_security_state_t* out_state);
MTK_API mtk_error_t mtk_service_get_capabilities(mtk_service_t* svc,
                                                 mtk_capabilities_t* out_caps);

MTK_API mtk_error_t mtk_service_read_partition_table(mtk_service_t* svc,
                                                     mtk_partition_info_t* parts,
                                                     size_t* inout_count);
MTK_API mtk_error_t mtk_service_read_partition(mtk_service_t* svc,
                                               const mtk_partition_info_t* part,
                                               const char* out_path);
MTK_API mtk_error_t mtk_service_write_partition(mtk_service_t* svc,
                                                const mtk_partition_info_t* part,
                                                const char* image_path);
MTK_API mtk_error_t mtk_service_erase_partition(mtk_service_t* svc,
                                                const mtk_partition_info_t* part);

#ifdef __cplusplus
}
#endif

#endif

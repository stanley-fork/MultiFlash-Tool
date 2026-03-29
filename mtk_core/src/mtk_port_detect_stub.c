#include "mtk/mtk_port_detect.h"

int mtk_port_detect(mtk_detected_port_t* ports, int max_ports) {
    (void)ports;
    (void)max_ports;
    return 0;
}

mtk_error_t mtk_port_detect_first(char* port_name, size_t name_size) {
    (void)port_name;
    (void)name_size;
    return MTK_E_NOT_FOUND;
}

int mtk_port_detect_has_port(const char* port_name) {
    (void)port_name;
    return 0;
}

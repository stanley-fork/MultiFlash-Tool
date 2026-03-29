#ifndef EDL_PORT_DETECT_H
#define EDL_PORT_DETECT_H

#include "edl_types.h"
#include "edl_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Detect Qualcomm EDL (9008) and DLOAD (9006) ports.
 * Fills ports[] up to max_ports. Returns number found.
 */
int edl_port_detect(edl_detected_port_t *ports, int max_ports);

/*
 * Detect and return the first 9008 EDL port name.
 * Returns EDL_OK if found, port_name filled. Returns EDL_ERR_PORT_NOT_FOUND otherwise.
 */
edl_error_t edl_port_detect_first_edl(char *port_name, size_t name_size);

/* True if port_name matches a currently present Qualcomm EDL(9008)/DLOAD(9006) COM port */
bool edl_port_detect_has_edl_port(const char *port_name);

#ifdef __cplusplus
}
#endif

#endif /* EDL_PORT_DETECT_H */

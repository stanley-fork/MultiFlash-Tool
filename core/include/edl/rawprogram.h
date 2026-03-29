#ifndef EDL_RAWPROGRAM_H
#define EDL_RAWPROGRAM_H

#include "edl_types.h"
#include "edl_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Parse rawprogram XML (rawprogram0.xml, rawprogram_unsparse.xml, etc).
 * Fills tasks[] up to max_tasks.
 * base_dir is prepended to filenames (pass "" if not needed).
 * Returns number of tasks parsed, or negative on error.
 */
int edl_rawprogram_parse(const char *xml_path, const char *base_dir,
                          edl_flash_task_t *tasks, int max_tasks);

/*
 * Parse patch XML (patch0.xml).
 * Returns number of patch entries, or negative on error.
 */
int edl_patch_parse(const char *xml_path,
                     edl_patch_entry_t *patches, int max_patches);

/*
 * When num_partition_sectors is 0 in XML (common for userdata / sparse),
 * infer sector count from image file (Android sparse or raw).
 * Only affects PROGRAM tasks with num_sectors == 0 and a valid filepath.
 */
void edl_flash_task_infer_sectors_from_image(edl_flash_task_t *t);

#ifdef __cplusplus
}
#endif

#endif /* EDL_RAWPROGRAM_H */

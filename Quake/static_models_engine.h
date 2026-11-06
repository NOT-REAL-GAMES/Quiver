/* Editor compiler and console diagnostics. */
#ifndef QUIVER_STATIC_MODELS_ENGINE_H
#define QUIVER_STATIC_MODELS_ENGINE_H
#include "static_models.h"
#include "editor_map.h"
void StaticModels_Init (void);
bool EditorStaticModels_Prepare (editor_map_t *build, uint8_t **lump, size_t *length,
	char *error, size_t error_size);
bool EditorStaticModels_Attach (const char *bsp_path, const uint8_t *lump, size_t length,
	char *error, size_t error_size);
#endif

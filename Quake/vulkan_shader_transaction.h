/* Roll back partially created shader modules when a required module fails.
 * Vulkan objects are supplied by callbacks so this bookkeeping can be used
 * with the renderer's existing diagnostics and exercised with fault injection. */
#ifndef QUIVER_VULKAN_SHADER_TRANSACTION_H
#define QUIVER_VULKAN_SHADER_TRANSACTION_H

#include <stdbool.h>
#include <stddef.h>
#include <vulkan/vulkan.h>

typedef VkShaderModule (*shader_transaction_create_fn)(void *context, const unsigned char *code, int size, const char *name);
typedef void (*shader_transaction_destroy_fn)(void *context, VkShaderModule module);

typedef struct shader_transaction_s
{
	VkShaderModule **created;
	size_t count, capacity;
	shader_transaction_create_fn create;
	shader_transaction_destroy_fn destroy;
	void *context;
} shader_transaction_t;

static inline bool ShaderTransaction_Create (shader_transaction_t *transaction, VkShaderModule *output,
	const unsigned char *code, int size, const char *name, bool required)
{
	if (!transaction || !transaction->create || !transaction->destroy || !output || *output != VK_NULL_HANDLE ||
		!transaction->created || transaction->count >= transaction->capacity)
		return false;
	VkShaderModule module = transaction->create (transaction->context, code, size, name);
	if (module == VK_NULL_HANDLE)
		return !required;
	*output = module;
	transaction->created[transaction->count++] = output;
	return true;
}

static inline void ShaderTransaction_Rollback (shader_transaction_t *transaction)
{
	while (transaction->count)
	{
		VkShaderModule *module = transaction->created[--transaction->count];
		transaction->destroy (transaction->context, *module);
		*module = VK_NULL_HANDLE;
	}
}
#endif

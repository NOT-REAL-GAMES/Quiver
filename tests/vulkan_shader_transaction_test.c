#include "../Quake/vulkan_shader_transaction.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

typedef struct fixture_s
{
	unsigned calls, fail_at, serial, live;
	bool handles[32];
} fixture_t;

static VkShaderModule create_module (void *context, const unsigned char *code, int size, const char *name)
{
	fixture_t *fixture = context;
	assert (code && size == 4 && name);
	if (++fixture->calls == fixture->fail_at) return VK_NULL_HANDLE;
	unsigned id = ++fixture->serial;
	assert (id < 32 && !fixture->handles[id]);
	fixture->handles[id] = true;
	++fixture->live;
	return (VkShaderModule)(uintptr_t)id;
}

static void destroy_module (void *context, VkShaderModule module)
{
	fixture_t *fixture = context;
	unsigned id = (unsigned)(uintptr_t)module;
	assert (id && id < 32 && fixture->handles[id]);
	fixture->handles[id] = false;
	--fixture->live;
}

static bool create_set (shader_transaction_t *transaction, VkShaderModule outputs[5])
{
	static const unsigned char code[4] = {0};
	for (unsigned i = 0; i < 5; ++i)
		if (!ShaderTransaction_Create (transaction, &outputs[i], code, sizeof (code), "fixture", !(i & 1)))
		{
			ShaderTransaction_Rollback (transaction);
			return false;
		}
	return true;
}

int main (void)
{
	for (unsigned fail_at = 0; fail_at <= 5; ++fail_at)
	{
		fixture_t fixture = {.fail_at = fail_at};
		VkShaderModule outputs[5] = {0}, *created[5];
		shader_transaction_t transaction = {created, 0, 5, create_module, destroy_module, &fixture};
		bool ok = create_set (&transaction, outputs);
		if (fail_at == 1 || fail_at == 3 || fail_at == 5)
		{
			assert (!ok && !fixture.live && !transaction.count);
			for (unsigned i = 0; i < 5; ++i) assert (!outputs[i]);
			fixture.calls = fixture.serial = fixture.fail_at = 0;
			assert (create_set (&transaction, outputs));
		}
		else
		{
			assert (ok && fixture.live == (fail_at ? 4u : 5u));
			if (fail_at) assert (!outputs[fail_at - 1]);
		}
		ShaderTransaction_Rollback (&transaction);
		assert (!fixture.live && !transaction.count);
		for (unsigned i = 0; i < 5; ++i) assert (!outputs[i]);
	}
	fixture_t fixture = {0};
	VkShaderModule occupied = (VkShaderModule)(uintptr_t)99, *created[1];
	shader_transaction_t transaction = {created, 0, 1, create_module, destroy_module, &fixture};
	static const unsigned char code[4] = {0};
	assert (!ShaderTransaction_Create (&transaction, &occupied, code, sizeof (code), "occupied", true));
	assert ((uintptr_t)occupied == 99 && fixture.calls == 0);
	VkShaderModule first = VK_NULL_HANDLE, second = VK_NULL_HANDLE;
	assert (ShaderTransaction_Create (&transaction, &first, code, sizeof (code), "first", true));
	assert (!ShaderTransaction_Create (&transaction, &second, code, sizeof (code), "capacity", false));
	assert (!second && fixture.calls == 1 && fixture.live == 1);
	ShaderTransaction_Rollback (&transaction);
	assert (!first && !fixture.live);
	puts ("PASS: required shader failures roll back modules; optional failures and retries are safe");
	return 0;
}

// Copyright 2022 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "module.h"

#include <cstdio>

#include "iree/base/api.h"
#include "iree/vm/api.h"
#include "iree/vm/native_module_cc.h"

// NOTE: this module is written in C++ using the native module wrapper and uses
// template magic to handle marshaling arguments. For a lot of uses this is a
// much friendlier way of exposing modules to the IREE VM and if performance and
// code size are not a concern is a fine route to take. Here we do it for
// brevity but all of the internal IREE modules are implemented in C.

//===----------------------------------------------------------------------===//
// !custom.string type
//===----------------------------------------------------------------------===//

// The "string" type we use to store and retain string data.
// This could be arbitrarily complex or simply wrap another user-defined type.
// The descriptor that is registered at startup defines how to manage the
// lifetime of the type (such as which destruction function is called, if any).
// See ref.h for more information and additional utilities.
typedef struct iree_custom_string_t {
  // Must be the first field; used to track the reference count of the object.
  iree_vm_ref_object_t ref_object;
  // Allocator the string data was allocated from.
  // Ideally pools and nested allocators would be used to avoid needing to store
  // the allocator with every object.
  iree_allocator_t allocator;
  // Non-NUL-terminated string value.
  iree_string_view_t value;
} iree_custom_string_t;

// Runtime type descriptor for the !custom.string describing how to manage it
// and destroy it. The type ID is allocated at runtime and does not need to
// match the compiler ID.
IREE_VM_DEFINE_TYPE_ADAPTERS(iree_custom_string, iree_custom_string_t);

extern "C" iree_status_t iree_custom_string_create(
    iree_string_view_t value, iree_allocator_t allocator,
    iree_custom_string_t** out_string) {
  IREE_ASSERT_ARGUMENT(out_string);
  // Note that we allocate the string and the string value together.
  iree_custom_string_t* string = NULL;
  IREE_RETURN_IF_ERROR(iree_allocator_malloc(
      allocator, sizeof(*string) + value.size, (void**)&string));
  string->ref_object.counter = IREE_ATOMIC_VAR_INIT(1);
  string->allocator = allocator;
  string->value.data = ((const char*)string) + sizeof(iree_custom_string_t);
  string->value.size = value.size;
  memcpy((void*)string->value.data, value.data, string->value.size);
  *out_string = string;
  return iree_ok_status();
}

extern "C" void iree_custom_string_destroy(void* ptr) {
  iree_custom_string_t* string = (iree_custom_string_t*)ptr;
  iree_allocator_free(string->allocator, ptr);
}

static iree_vm_ref_type_descriptor_t iree_custom_string_descriptor_storage = {
    0};

// Registers types provided by the custom module.
// We must call this before any of our types can be resolved.
iree_status_t iree_custom_module_basic_register_types(
    iree_vm_instance_t* instance) {
  iree_custom_string_descriptor_storage.destroy = iree_custom_string_destroy;
  iree_custom_string_descriptor_storage.type_name = IREE_SV("custom.string");
  iree_custom_string_descriptor_storage.offsetof_counter =
      offsetof(iree_custom_string_t, ref_object.counter) /
      IREE_VM_REF_COUNTER_ALIGNMENT;
  return iree_vm_instance_register_type(instance,
                                        &iree_custom_string_descriptor_storage,
                                        &iree_custom_string_registration);
}

static void iree_custom_module_basic_unregister_types(
    iree_vm_instance_t* instance) {
  iree_vm_instance_unregister_type(instance,
                                   &iree_custom_string_descriptor_storage);
}

//===----------------------------------------------------------------------===//
// VM module interface implementation
//===----------------------------------------------------------------------===//

namespace {

using namespace iree;

// Per-context module state.
// This can contain "globals" and other arbitrary state.
//
// Thread-compatible; the runtime will not issue multiple calls at the same
// time using the same state. If the implementation uses external threads then
// it must synchronize itself.
class CustomModuleState final {
 public:
  explicit CustomModuleState(iree_allocator_t host_allocator)
      : host_allocator_(host_allocator) {}
  ~CustomModuleState() = default;

  // Creates a new string with a copy of the given string data.
  // No NUL terminator is required.
  StatusOr<vm::ref<iree_custom_string_t>> StringCreate(
      iree_string_view_t data) {
    vm::ref<iree_custom_string_t> string;
    IREE_RETURN_IF_ERROR(
        iree_custom_string_create(data, host_allocator_, &string));
    fprintf(stdout, "CREATE %.*s\n", static_cast<int>(string->value.size),
            string->value.data);
    fflush(stdout);
    return std::move(string);
  }

  // Returns the length of the string in characters.
  StatusOr<int64_t> StringLength(const vm::ref<iree_custom_string_t> string) {
    if (!string) {
      // Passed in refs may be null.
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT, "null string arg");
    }
    fprintf(stdout, "LENGTH %.*s = %" PRIhsz "\n",
            static_cast<int>(string->value.size), string->value.data,
            string->value.size);
    fflush(stdout);
    return static_cast<int64_t>(string->value.size);
  }

  // Prints the contents of the string to stdout.
  Status StringPrint(const vm::ref<iree_custom_string_t> string) {
    if (!string) return OkStatus();  // no-op
    fprintf(stdout, "PRINT %.*s\n", static_cast<int>(string->value.size),
            string->value.data);
    fflush(stdout);
    return OkStatus();
  }

  // Prints the contents of the string; only exported in debug mode.
  Status StringDPrint(const vm::ref<iree_custom_string_t> string) {
    if (!string) return OkStatus();  // no-op
    return StringPrint(std::move(string));
  }

 private:
  // Allocator that the caller requested we use for any allocations we need to
  // perform during operation.
  iree_allocator_t host_allocator_;
};

// Function table mapping imported function names to their implementation.
static const vm::NativeFunction<CustomModuleState> kCustomModuleFunctions[] = {
    vm::MakeNativeFunction("string.create", &CustomModuleState::StringCreate),
    vm::MakeNativeFunction("string.length", &CustomModuleState::StringLength),
    vm::MakeNativeFunction("string.print", &CustomModuleState::StringPrint),

#if !NDEBUG
    // This is an optional method that we purposefully compile out in release
    // builds to demonstrate fallback paths. Consuming modules can query as to
    // whether the function exists at runtime and call fallbacks/change their
    // behavior.
    vm::MakeNativeFunction("string.dprint", &CustomModuleState::StringDPrint),
#endif  // !NDEBUG
};

// The module instance that will be allocated and reused across contexts.
// Any context-specific state must be stored in a state structure such as
// CustomModuleState.
//
// Assumed thread-safe (by construction here, as it's immutable), though if any
// mutable state is stored here it will need to be synchronized by the
// implementation.
class CustomModule final : public vm::NativeModule<CustomModuleState> {
 public:
  using vm::NativeModule<CustomModuleState>::NativeModule;

  ~CustomModule() override {
    iree_custom_module_basic_unregister_types(instance());
  }

  // Creates per-context state when the module is added to a new context.
  // May be called from any thread.
  StatusOr<std::unique_ptr<CustomModuleState>> CreateState(
      iree_allocator_t allocator) override {
    auto state = std::make_unique<CustomModuleState>(allocator);
    return state;
  }

  // Forks a parent state into a child state, preserving any module state
  // by-reference.
  StatusOr<std::unique_ptr<CustomModuleState>> ForkState(
      CustomModuleState* parent_state,
      iree_allocator_t host_allocator) override {
    // No special state to preserve.
    return CreateState(host_allocator);
  }
};

}  // namespace

// Note that while we are using C++ bindings internally we still expose the
// module as a C instance. This hides the details of our implementation.
extern "C" iree_status_t iree_custom_module_basic_create(
    iree_vm_instance_t* instance, iree_allocator_t allocator,
    iree_vm_module_t** out_module) {
  IREE_ASSERT_ARGUMENT(out_module);
  *out_module = NULL;

  // Register the types used by the module.
  // The CustomModule destructor will unregister them when it's done.
  // Unregistration isn't strictly required in some cases but is good practice.
  iree_custom_module_basic_register_types(instance);

  // NOTE: this isn't using the allocator here and that's bad as it leaves
  // untracked allocations and pulls in the system allocator that may differ
  // from the one requested by the user.
  // TODO(benvanik): std::allocator wrapper around iree_allocator_t so this can
  // use that instead.
  auto module = std::make_unique<CustomModule>(
      "custom", /*version=*/0, instance, allocator,
      iree::span<const vm::NativeFunction<CustomModuleState>>(
          kCustomModuleFunctions));

  *out_module = module.release()->interface();
  return iree_ok_status();
}

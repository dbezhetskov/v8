#include "src/cloudflare/isolate-pool.h"

#include <signal.h>
#include <sys/mman.h>
// `sys/mman.h defines `MAP_TYPE`, but `MAP_TYPE` also gets defined within V8.
// Since we don't need `sys/mman.h`'s `MAP_TYPE`, we undefine it immediately
// after the `#include`.
#undef MAP_TYPE
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <set>
#include <thread>

#include "include/v8-context.h"
#include "include/v8-exception.h"
#include "include/v8-function.h"
#include "include/v8-initialization.h"
#include "include/v8-isolate.h"
#include "include/v8-local-handle.h"
#include "include/v8-script.h"
#include "include/v8-template.h"
#include "src/base/logging.h"
#include "src/common/assert-scope.h"
#include "src/common/globals.h"
#include "src/execution/isolate.h"
#include "src/heap/heap.h"

namespace {

constexpr v8::EmbedderDataTypeTag kApiMyCppObjectTag = 3;

const size_t kPageSize = sysconf(_SC_PAGESIZE);
constexpr size_t kMaxPosition = 512;
thread_local uintptr_t g_dirt_buffer[kMaxPosition];
thread_local size_t g_current_records_position = 0;

void pages_signal_handler(int sig, siginfo_t* info, void* context) {
  if (sig != SIGSEGV) {
    signal(sig, SIG_DFL);
  }

  auto* uctx = static_cast<ucontext_t*>(context);
  auto error_code = uctx->uc_mcontext.gregs[REG_ERR];
  bool is_write = error_code & 0x2;
  if (!is_write) {
    FATAL("Unknown access");
  }

  uintptr_t addr = reinterpret_cast<uintptr_t>(info->si_addr);
  uintptr_t page_base = addr & ~(kPageSize - 1);

  mprotect(reinterpret_cast<void*>(page_base), kPageSize,
           PROT_READ | PROT_WRITE);

  size_t pos = g_current_records_position;
  if (pos >= kMaxPosition) {
    FATAL("Please increase kMaxPosition");
  }

  g_dirt_buffer[pos] = page_base;
  g_current_records_position = pos + 1;
}

static struct sigaction old_signal_action;

void install_signal_handler() {
  struct sigaction sa;
  sa.sa_sigaction = pages_signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_SIGINFO;
  if (sigaction(SIGSEGV, &sa, &old_signal_action) == -1) {
    FATAL("Could not install SIGSEGV page signal handler");
  }
}

void uninstall_signal_handler() {
  if (sigaction(SIGSEGV, &old_signal_action, NULL) == -1) {
    FATAL("Could not uninstall SIGSEGV page signal handler");
  }
}

void SayHelloFromJS(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Local<v8::Object> self = args.This();
  auto ptr = static_cast<MyExternalObject*>(
      self->GetAlignedPointerFromInternalField(0, kApiMyCppObjectTag));
  if (ptr) {
    ptr->SayHello();
  }
}

intptr_t external_references[] = {reinterpret_cast<intptr_t>(SayHelloFromJS),
                                  0};

v8::Local<v8::Object> WrapMyExternalObject(v8::Isolate* isolate,
                                           MyExternalObject* obj) {
  // Create a template with 1 internal field to hold the C++ pointer.
  v8::Local<v8::ObjectTemplate> obj_template = v8::ObjectTemplate::New(isolate);
  obj_template->SetInternalFieldCount(1);
  obj_template->Set(isolate, "sayHello",
                    v8::FunctionTemplate::New(isolate, SayHelloFromJS));

  // Instantiate the object and attach the pointer.
  v8::Local<v8::Object> js_obj =
      obj_template->NewInstance(isolate->GetCurrentContext()).ToLocalChecked();
  js_obj->SetAlignedPointerInInternalField(0, obj, kApiMyCppObjectTag);
  return js_obj;
}

IsolatePool::JSEnvironment CreateJSEnvironment(v8::Isolate* isolate,
                                               const char* code) {
  v8::Isolate::Scope isolate_scope(isolate);
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Context> context = v8::Context::New(isolate);
  v8::Context::Scope context_scope(context);

  // Install external objects.
  std::vector<v8::Global<v8::Object>> external_objects;
  v8::Local<v8::Object> js_wrapper_for_ext_obj =
      WrapMyExternalObject(isolate, new MyExternalObject(42));
  context->Global()
      ->Set(context, v8::String::NewFromUtf8Literal(isolate, "myExternalObj"),
            js_wrapper_for_ext_obj)
      .Check();
  external_objects.push_back(
      v8::Global<v8::Object>(isolate, js_wrapper_for_ext_obj));

  v8::Local<v8::String> source =
      v8::String::NewFromUtf8(isolate, code, v8::NewStringType::kNormal)
          .ToLocalChecked();
  v8::Local<v8::Script> script =
      v8::Script::Compile(context, source).ToLocalChecked();
  script->Run(context).ToLocalChecked();

  v8::Local<v8::String> key = v8::String::NewFromUtf8Literal(isolate, "main");
  v8::Local<v8::Value> val;
  CHECK(context->Global()->Get(context, key).ToLocal(&val) &&
        val->IsFunction());
  return IsolatePool::JSEnvironment{
      v8::Global<v8::Context>(isolate, context),
      v8::Global<v8::Function>(isolate, val.As<v8::Function>()),
      std::move(external_objects)};
}

}  // namespace

IsolatePool::IsolatePool(const char* runtime_code, size_t pool_size,
                         TrackingAccess tracking_access_support)
    : cloned_group_(v8::IsolateGroup::GetDefault()),
      original_group_(v8::IsolateGroup::GetDefault()),
      tracking_access_support_(tracking_access_support) {
  // Create isolates and clone them.
  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator =
      v8::ArrayBuffer::Allocator::NewDefaultAllocator(original_group_);
  create_params.external_references = external_references;

  std::vector<std::pair<v8::Isolate*, JSEnvironment>> original_isolate_contexts;
  for (size_t i = 0; i < pool_size; ++i) {
    v8::Isolate* isolate = v8::Isolate::New(original_group_, create_params);
    original_isolate_contexts.emplace_back(
        isolate, CreateJSEnvironment(isolate, runtime_code));
  }

  // TODO(dbezhetskov): think about this.
  // Now we are loosing all young objects with GC.
  // RunGCForIsolates(&original_isolate_contexts);
  original_group_.Freeze();

  // Clone original group with all isolates inside.
  cloned_group_ = original_group_.Clone();
  for (size_t i = 0; i < original_isolate_contexts.size(); ++i) {
    v8::Isolate* original_isolate = original_isolate_contexts[i].first;
    JSEnvironment original_env = std::move(original_isolate_contexts[i].second);
    v8::Isolate* cloned_isolate =
        v8::Isolate::MaterializeClone(original_isolate, cloned_group_);
    JSEnvironment cloned_env;
    {
      v8::Isolate::Scope isolate_scope(cloned_isolate);
      cloned_env.context = v8::Global<v8::Context>(
          original_isolate, cloned_isolate, &original_env.context);
      cloned_env.main = v8::Global<v8::Function>(
          original_isolate, cloned_isolate, &original_env.main);
      for (auto& original_ext_obj_wrapper : original_env.external_objects) {
        cloned_env.external_objects.push_back(v8::Global<v8::Object>(
            original_isolate, cloned_isolate, &original_ext_obj_wrapper));
      }
    }
    isolates_and_ctxs_.emplace_back(original_isolate, std::move(original_env),
                                    cloned_isolate, std::move(cloned_env));

    initial_pool_.push_back(
        PoolRecord{cloned_isolate, original_isolate,
                   &isolates_and_ctxs_.back().cloned_context.context,
                   &isolates_and_ctxs_.back().cloned_context.main,
                   &isolates_and_ctxs_.back().cloned_context.external_objects});
    stack_.push(
        PoolRecord{cloned_isolate, original_isolate,
                   &isolates_and_ctxs_.back().cloned_context.context,
                   &isolates_and_ctxs_.back().cloned_context.main,
                   &isolates_and_ctxs_.back().cloned_context.external_objects});
  }

  if (IsPagesTrackingSupported()) {
    cloned_group_.SetReadOnlyPermissionForSandbox();
    install_signal_handler();
  }
}

IsolatePool::~IsolatePool() {
  for (auto& [original_isolate, original_context, cloned_isolate,
              cloned_context] : isolates_and_ctxs_) {
    cloned_context.context.Reset();
    cloned_context.main.Reset();
    for (auto& wrapper : cloned_context.external_objects) {
      wrapper.Reset();
    }
    cloned_isolate->Dispose();

    original_context.context.Reset();
    original_context.main.Reset();
    for (auto& wrapper : original_context.external_objects) {
      wrapper.Reset();
    }
    original_isolate->Dispose();
  }

  if (IsPagesTrackingSupported()) {
    uninstall_signal_handler();
  }
}

std::optional<IsolatePool::PoolRecord> IsolatePool::GetIsolate() {
  if (stack_.empty()) {
    return std::nullopt;
  }

  PoolRecord record = stack_.top();
  stack_.pop();
  return record;
}

void IsolatePool::FreeIsolate(PoolRecord record) {
  if (!IsPagesTrackingSupported()) {
    // Do nothing.
    return;
  }

  // Find all dirty pages belonging to this isolate
  // and remove dirt from them.
  size_t current_pos = g_current_records_position;
  for (size_t i = 0; i < current_pos; ++i) {
    char* dirt_page_base = reinterpret_cast<char*>(g_dirt_buffer[i]);
    char* original_page_base =
        original_group_.GetPointerCageBase() +
        (dirt_page_base - cloned_group_.GetPointerCageBase());
    std::memcpy(dirt_page_base, original_page_base, kPageSize);
  }

  // Reset buffer pos for the next isolate.
  g_current_records_position = 0;

  stack_.push(std::move(record));
}

void IsolatePool::FullReset() {
  cloned_group_.ResetClone();

  // Clear and refill the stack.
  // TODO(dbezhetskov): remove initial_pool_ and check here that stack_.size ==
  // number of cloned isolates.
  std::stack<PoolRecord>().swap(stack_);
  for (const auto& pool_record : initial_pool_) {
    stack_.push(pool_record);
  }
}

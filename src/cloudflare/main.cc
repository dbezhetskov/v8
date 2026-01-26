// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <include/libplatform/libplatform.h>

#include <chrono>
#include <format>
#include <fstream>
#include <iostream>
#include <streambuf>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "include/v8-context.h"
#include "include/v8-exception.h"
#include "include/v8-function.h"
#include "include/v8-initialization.h"
#include "include/v8-isolate.h"
#include "include/v8-local-handle.h"
#include "include/v8-script.h"
#include "include/v8-snapshot.h"
#include "include/v8-template.h"
#include "src/base/logging.h"
#include "src/cloudflare/isolate-pool.h"
#include "src/common/assert-scope.h"
#include "src/common/globals.h"
#include "src/execution/isolate.h"
#include "src/heap/heap.h"

constexpr v8::EmbedderDataTypeTag kApiMyCppObjectTag = 3;
MyExternalObject* cpp_obj = new MyExternalObject(42);

static void SayHelloFromJS(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Local<v8::Object> self = args.This();
  auto ptr = static_cast<MyExternalObject*>(
      self->GetAlignedPointerFromInternalField(0, kApiMyCppObjectTag));
  if (ptr) {
    ptr->SayHello();
  }
}

intptr_t external_references[] = {reinterpret_cast<intptr_t>(SayHelloFromJS),
                                  0};

struct ScopedTimer {
  explicit ScopedTimer(std::string message)
      : begin_(std::chrono::steady_clock::now()),
        message_(std::move(message)) {}

  ~ScopedTimer() {
    std::chrono::steady_clock::time_point end =
        std::chrono::steady_clock::now();
    std::cout << message_ << " "
              << std::chrono::duration_cast<std::chrono::microseconds>(end -
                                                                       begin_)
                     .count()
              << "[µs]" << std::endl;
  }

  std::chrono::steady_clock::time_point begin_;
  std::string message_;
};

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

v8::StartupData SerializeInternalFields(v8::Local<v8::Object> holder, int index,
                                        void*) {
  if (index != 0 || holder->InternalFieldCount() <= 0) {
    return {nullptr, 0};
  }

  MyExternalObject* cpp_native_object_pointer =
      reinterpret_cast<MyExternalObject*>(
          holder->GetAlignedPointerFromInternalField(0, kApiMyCppObjectTag));
  if (!cpp_native_object_pointer) {
    return {nullptr, 0};
  }

  constexpr size_t size_in_bytes = sizeof(MyExternalObject*);
  // Copy raw pointer because the cpp object outlives deserialization.
  char* payload = new char[size_in_bytes];
  std::memcpy(payload, reinterpret_cast<void*>(&cpp_native_object_pointer),
              sizeof(MyExternalObject*));
  return {payload, size_in_bytes};
}

void DeserializeInternalFields(v8::Local<v8::Object> holder, int index,
                               v8::StartupData payload, void*) {
  if (index != 0 || holder->InternalFieldCount() <= 0) return;

  if (payload.data == nullptr ||
      payload.raw_size != static_cast<int>(sizeof(MyExternalObject*))) {
    // Nothing serialized for this field.
    holder->SetAlignedPointerInInternalField(0, nullptr, kApiMyCppObjectTag);
    return;
  }

  MyExternalObject* object_ptr = nullptr;
  std::memcpy(&object_ptr, payload.data, payload.raw_size);
  holder->SetAlignedPointerInInternalField(0, object_ptr, kApiMyCppObjectTag);
}

v8::StartupData CreateSnapshot(const char* source_code) {
  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator =
      v8::ArrayBuffer::Allocator::NewDefaultAllocator();
  create_params.external_references = external_references;

  v8::StartupData snapshot_data;
  {
    v8::SnapshotCreator creator(create_params);
    auto isolate = creator.GetIsolate();

    v8::HandleScope handle_scope(isolate);
    v8::Local<v8::Context> context = v8::Context::New(isolate);

    v8::Context::Scope context_scope(context);
    v8::Local<v8::Object> js_obj = WrapMyExternalObject(isolate, cpp_obj);
    context->Global()
        ->Set(context, v8::String::NewFromUtf8Literal(isolate, "myExternalObj"),
              js_obj)
        .Check();
    v8::Local<v8::String> source =
        v8::String::NewFromUtf8(isolate, source_code).ToLocalChecked();
    v8::Local<v8::Script> compiled_script =
        v8::Script::Compile(context, source).ToLocalChecked();
    compiled_script->Run(context).ToLocalChecked();

    creator.SetDefaultContext(context, v8::SerializeInternalFieldsCallback(
                                           SerializeInternalFields, nullptr));
    snapshot_data =
        creator.CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kKeep);
  }

  delete create_params.array_buffer_allocator;
  return snapshot_data;
}

void RunJSHelper(v8::Isolate* isolate, v8::Local<v8::Function>* main_fn,
                 v8::Local<v8::Context>* context) {
  v8::Local<v8::Value> result;
  if (!(*main_fn)
           ->Call(*context, (*context)->Global(), 0, nullptr)
           .ToLocal(&result)) {
    std::cerr << "Call to main failed" << std::endl;
    return;
  }

  if (result->IsPromise()) {
    v8::Local<v8::Promise> promise = result.As<v8::Promise>();

    while (promise->State() == v8::Promise::kPending) {
      isolate->PerformMicrotaskCheckpoint();
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (promise->State() == v8::Promise::kFulfilled) {
      v8::String::Utf8Value utf8(isolate, promise->Result());
      std::cout << "Promise fulfilled with: " << *utf8 << std::endl;
    } else {
      v8::String::Utf8Value utf8(isolate, promise->Result());
      std::cerr << "Promise rejected with: " << *utf8 << std::endl;
    }
  } else {
    // Convert the result to an UTF8 string and print it.
    v8::String::Utf8Value utf8(isolate, result);
    std::cout << "Script result: " << *utf8 << std::endl;
  }
}

void RunJSSnapshot(v8::Isolate* isolate) {
  v8::Isolate::Scope isolate_scope(isolate);
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Context> context =
      v8::Context::New(isolate, nullptr, v8::MaybeLocal<v8::ObjectTemplate>(),
                       v8::MaybeLocal<v8::Value>(),
                       v8::DeserializeInternalFieldsCallback(
                           DeserializeInternalFields, nullptr));
  v8::Context::Scope context_scope(context);
  v8::Local<v8::String> main_fn_name =
      v8::String::NewFromUtf8Literal(isolate, "main");
  v8::Local<v8::Value> val;
  if (!context->Global()->Get(context, main_fn_name).ToLocal(&val) ||
      !val->IsFunction()) {
    std::cerr << "Cant find main function in the context" << std::endl;
    return;
  }
  v8::Local<v8::Function> main_fn = val.As<v8::Function>();
  RunJSHelper(isolate, &main_fn, &context);
}

void RunJS(v8::Isolate* isolate, v8::Global<v8::Context>* ctx,
           v8::Global<v8::Function>* main,
           std::vector<v8::Global<v8::Object>>* external_objects) {
  v8::Isolate::Scope isolate_scope(isolate);
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Context> context = ctx->Get(isolate);
  v8::Context::Scope context_scope(context);
  v8::Local<v8::Function> main_fn = main->Get(isolate);

  // Recreate external objects for cloned isolate.
  std::vector<v8::Local<v8::Object>> external_local_wrappers;
  for (v8::Global<v8::Object>& ext_obj_wrapper : *external_objects) {
    external_local_wrappers.push_back(ext_obj_wrapper.Get(isolate));
    external_local_wrappers.back()->SetAlignedPointerInInternalField(
        0, new MyExternalObject(rand()), kApiMyCppObjectTag);
  }

  RunJSHelper(isolate, &main_fn, &context);
}

int main(int argc, char* argv[]) {
  if (argc != 3) {
    std::cerr << "Wrong number of arguments <source_path> snapshot|mmap"
              << std::endl;
    return EXIT_FAILURE;
  } else if (std::string(argv[2]) != "snapshot" &&
             std::string(argv[2]) != "mmap") {
    std::cerr << "Wrong arguments, should be snapshot or mmap" << std::endl;
    return EXIT_FAILURE;
  }

  const bool testing_snapshot = std::string(argv[2]) == "snapshot";

  std::ifstream t(argv[1]);
  std::string js_runtime_code((std::istreambuf_iterator<char>(t)),
                              std::istreambuf_iterator<char>());

  // Initialize V8.
  v8::V8::InitializeICUDefaultLocation(argv[0]);
  v8::V8::InitializeExternalStartupData(argv[0]);
  std::unique_ptr<v8::Platform> platform = v8::platform::NewDefaultPlatform();
  v8::V8::InitializePlatform(platform.get());
  v8::V8::SetFlagsFromString("--expose-gc");
  v8::V8::SetFlagsFromString("--no-short-builtin-calls");
  v8::V8::SetFlagsFromString("--verify-heap");
  v8::V8::SetFlagsFromString("--verify-write-barriers");
  v8::V8::Initialize();

  if (!v8::IsolateGroup::CanCreateNewGroups()) {
    std::cerr << "Can't create new groups" << std::endl;
    return EXIT_FAILURE;
  }

  v8::Isolate::CreateParams create_params;
  if (testing_snapshot) {
    // Testing traditional approach with snapshoting.
    auto snapshot_blob = CreateSnapshot(js_runtime_code.c_str());
    create_params.snapshot_blob = &snapshot_blob;

    create_params.array_buffer_allocator =
        v8::ArrayBuffer::Allocator::NewDefaultAllocator();
    create_params.external_references = external_references;
    v8::Isolate* isolate = nullptr;
    {
      ScopedTimer timer("Isolate new with snapshot");
      isolate = v8::Isolate::New(create_params);
    }

    {
      ScopedTimer timer("Run JS time:");
      RunJSSnapshot(isolate);
    }

    isolate->Dispose();
  } else {
    // Testing COW/mmap approach.
    constexpr size_t kPoolSize = 1;
    IsolatePool pool(js_runtime_code.c_str(), kPoolSize,
                     IsolatePool::TrackingAccess::NO_TRACKING_SUPPORTED);

    constexpr size_t kNumOfRequests = 1;
    {
      ScopedTimer timer(
          std::format("Time to handle {} requests:", kNumOfRequests));
      for (size_t i = 0; i < kNumOfRequests; ++i) {
        auto pool_record_opt = pool.GetIsolate();
        DCHECK(pool_record_opt);
        RunJS(pool_record_opt->isolate, pool_record_opt->context,
              pool_record_opt->main, pool_record_opt->external_objects);
        pool.FreeIsolate(std::move(*pool_record_opt));
      }
    }
  }

  // Tear down V8.
  v8::V8::Dispose();
  v8::V8::DisposePlatform();
  delete create_params.array_buffer_allocator;
  return 0;
}

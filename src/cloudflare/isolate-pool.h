#ifndef ISOLATE_POOL_H
#define ISOLATE_POOL_H

#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <stack>
#include <utility>
#include <vector>

#include "include/v8-isolate.h"

class IsolatePool final {
 public:
  enum class TrackingAccess : bool {
    TRACKING_SUPPORTED,
    NO_TRACKING_SUPPORTED
  };

  struct PoolRecord {
    v8::Isolate* isolate;
    v8::Isolate* underlying_isolate;
    v8::Global<v8::Context>* context;
    v8::Global<v8::Function>* main;
    std::vector<v8::Global<v8::Object>>* external_objects;
  };

  struct JSEnvironment {
    v8::Global<v8::Context> context;
    v8::Global<v8::Function> main;
    std::vector<v8::Global<v8::Object>> external_objects;
  };

  IsolatePool(const char* runtime_code, size_t pool_size,
              TrackingAccess tracking_access_support);

  ~IsolatePool();

  std::optional<PoolRecord> GetIsolate();

  void FreeIsolate(PoolRecord record);

  void FullReset();

 private:
  struct InternalRecord {
    v8::Isolate* original_isolate;
    JSEnvironment original_context;
    v8::Isolate* cloned_isolate;
    JSEnvironment cloned_context;
  };

  bool IsPagesTrackingSupported() const {
    return tracking_access_support_ == TrackingAccess::TRACKING_SUPPORTED;
  }

  v8::IsolateGroup cloned_group_;
  v8::IsolateGroup original_group_;
  std::list<InternalRecord> isolates_and_ctxs_;
  std::stack<PoolRecord> stack_;
  std::vector<PoolRecord> initial_pool_;
  TrackingAccess tracking_access_support_ =
      TrackingAccess::NO_TRACKING_SUPPORTED;
};

class MyExternalObject {
 public:
  explicit MyExternalObject(int value) : value_(value) {}
  void SayHello() const {
    std::cout << "Hello from C++, value = " << value_ << std::endl;
  }

 private:
  int value_;
};

#endif  // ISOLATE_POOL_H
